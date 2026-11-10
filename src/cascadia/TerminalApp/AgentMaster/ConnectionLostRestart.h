// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the AUTOMATIC RESTART after a lost connection (HOOKS.md "Connection lost mid-response — the automatic
// restart"). The user's request of 2026-11-08: "Lets add self healing for error "● API Error: Connection lost
// mid-response. The response above may be incomplete." by restarting session as uninterrupted automatically. As soon as
// this error state is detected, we shall restart the tab automatically (without switching to it and without opening a
// prompt asking for interrupted or uninterrupted restart)".
//
// WHAT CLAUDE CODE WRITES (2.1.310, read from its bundle and from real transcripts). When the response stream dies after
// output began, Claude Code finalizes the half-streamed reply and appends the synthetic row
//     {"type":"assistant", …"content":[{"type":"text","text":"API Error: Connection lost mid-response. The response above may
//      be incomplete."}]…, "error":"server_error", "truncatedAfterOutput":true, "isApiErrorMessage":true,
//      "streamCutCause":"stale_connection"}
// (the same text for the cut causes "stale_connection", "network_down", "stream_truncated" and a "stream_suspended" that
// was not a sleep; 2.1.282 / 2.1.291 wrote it without "streamCutCause"). The scanner turns it into Error (recon-error),
// and the claude then waits at the error until someone types — a restart alone does not help either: the half-reply
// above the error makes Claude Code's resume call the turn complete. Only "Restart uninterrupted"
// (RestartTurnChoice::LetTurnContinue — the API-error take-back line plus CLAUDE_CODE_RESUME_INTERRUPTED_TURN=1,
// ApiErrorRewind.h / InterruptedTurns.h) continues the turn.
//
// EVERY CUT-OFF RESPONSE, NOT ONLY THAT ONE (2026-11-10: the user's "● API Error: The response stopped arriving. The response
// above may be incomplete." — session 0c947da9 — "is now stuck and did not self heal"). The same code path writes one row per
// way the stream died, and each leaves the claude waiting at the error the same way, healed by the same restart. The
// feature keeps its first error's name ("ConnectionLost…", the public setting autoRestartOnConnectionLost) and covers them
// all (IsCutOffResponseError):
//   * after output began, every text ending "The response above may be incomplete." — Connection lost mid-response, The
//     response stopped arriving (the stream watchdog: streamCutCause "watchdog"), Server error mid-response, Part of the
//     response never arrived, The response stream was malformed, Your computer went to sleep mid-response. Claude Code tells
//     a partial response by that very sentence (its print mode: `result.endsWith("The response above may be incomplete.")`);
//   * before any output, "… before a response was produced. Try again." / "… and no response was produced. Try again." — no
//     half-reply to take back, but the claude waits at the error all the same, and the restart's resume sends the turn again.
//
// TEMPORARY API FAILURES TOO (2026-11-10, the user: "Search claude code for other variants of errors we should recover from,
// also consider accepting part of the message and not exact message"). Claude Code retries a failed request by itself and
// only then writes an error row — 2.1.310 retries up to 10 times by default, CLAUDE_CODE_MAX_RETRIES is clamped to 15 (the
// 50000 Agentmaster ships included), a 529 is retried 3 times — so the row lands after minutes of retries, and the claude
// then waits at it forever, exactly like a cut-off one; the same restart continues the turn. Claude Code's own /goal feature
// retries that very set by itself (its turn classifier: overloaded, server_error, a transient flag or an unclassified error ->
// "retry", after 1, 5 and 15 minutes, 3 times; rate and usage limits -> "pause"; auth, billing, a missing model, a context
// limit -> "stop"). So the automatic restart heals two KINDS of API error (ClassifyAutoRestartError, AutoRestartErrorKind):
//   * CutOff — the stream cuts above (IsCutOffResponseError): restarted AT ONCE;
//   * Transient — a server-side or network failure that outlived Claude Code's own retries: "… This is a server-side issue,
//     usually temporary …" (every 5xx), "Repeated 529 Overloaded errors", "Internal server error", "Server is temporarily
//     limiting requests (not your usage limit)", "Connection error." / "Connection dropped (ECONNRESET)" / "Unable to connect
//     to API (ECONNREFUSED)" / "Request timed out" / "No response from API", "The socket connection was closed unexpectedly",
//     "Stream idle timeout", and the empty-response / unreadable-tool-call retries that also failed — restarted after a
//     BACK-OFF (kTransientRestartBackoffMs: Claude Code's own /goal steps, 1, 5 and 15 minutes, each lengthened by a fixed
//     0–20 % share per session so one outage does not restart a whole fleet in the same second), at most
//     kTransientRestartBudget times within kTransientRestartWindowMs.
// Both are matched by PHRASES — case-insensitive ASCII words anywhere in the text, never the exact message — and the HTTP
// status (the row's apiErrorStatus, else an "API Error: NNN" in the text) counts too: 408 and every 5xx are temporary failures
// whatever their words. NEVER restarted, checked FIRST (kAutoRestartNeverPhrases, and a 4xx status but 408 / 429): every
// error that needs the user — a usage or session limit, usage credits, a billing error, /login, an API key or credentials,
// /model ("Opus is experiencing high load, please use /model to switch to Sonnet" included), a refusal ("safeguards"), a prompt
// too long, a context limit, the output token maximum, a certificate problem — and every wording neither list knows.
//
// WHAT THIS ADDS. The window hosting such a session's tab does that restart BY ITSELF — no question, no tab switch
// (TerminalPage.AgentTurns.cpp, "the AUTOMATIC RESTART after a lost connection"). The rules are pure and live here so
// the m5 harness pins them (TestConnectionLostRestart, tests_rewind.cpp):
//   * ClassifyAutoRestartError — which API errors it heals (CutOff / Transient) and which it never touches (None);
//   * DecideConnectionLostRestart — the REGISTRY gate, run on every change of a session the window hosts (no I/O): is it
//     an error the restart heals, on a tab Agentmaster can safely restart, with nothing a restart would destroy (an unsent
//     draft, a delivery in flight, background work), and — for a temporary failure — has its back-off passed?
//   * DecideConnectionLostRestartFromPlan — the TRANSCRIPT gate, one tail read per Error episode (off the UI thread):
//     restart only when the take-back really continues the turn, at most once per error row, and at most
//     kConnectionLostRestartBudget times per session within kConnectionLostRestartWindowMs for a cut-off response
//     (kTransientRestartBudget within kTransientRestartWindowMs for a temporary failure) — a network that keeps dropping or
//     a server that stays down must not turn into a restart loop;
//   * DecideConnectionLostRestartOutcome — after the restart: healed (the turn runs again), not healed, or still waiting;
//   * ShouldGiveRunnerBackAfterRestart — the Prompt Queue Runner the stop-on-error backstop paused gets its mode back once
//     the turn runs again (a healed turn must not leave its queue paused);
//   * ConnectionLostRestartLedger — the process-wide memory of the restarts made (one engine per process, Rule #10).
//
// Header-only. Plain C++ (no Windows, no WinRT): the app and the m5 harness include it.

#pragma once

#include <algorithm>
#include <cstdint>
#include <iterator> // std::size — the back-off table's length is the temporary-failure budget
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "ApiErrorRewind.h" // ApiErrorRewindPlan, ApiErrorTakeBackContinues — what the transcript gate reads
#include "SessionModels.h" // SessionState, AutorunnerState — what the registry gate and the runner give-back read

namespace Agentmaster
{
    // The sentence Claude Code ends every error row of a response cut off AFTER output began with ("API Error: Connection
    // lost mid-response. The response above may be incomplete.", "… The response stopped arriving. …", "… Server error
    // mid-response. …", …) — and the one its own print mode tells a partial result by. Matched without its final period.
    inline constexpr std::wstring_view kCutOffResponseMidStreamText = L"The response above may be incomplete";

    // The word three of those rows name the cut by ("Connection lost mid-response", "Server error mid-response", "Your computer
    // went to sleep mid-response") — matched too, so those stay recognized should a later Claude Code drop the sentence above.
    // Only Claude Code's stream-cut templates use it.
    inline constexpr std::wstring_view kCutOffResponseMidResponseText = L"mid-response";

    // The words of the error rows of a stream cut off BEFORE any output ("API Error: Connection lost before a response was
    // produced. Try again.", "… The response stalled before a response was produced. …", "… Your computer went to sleep
    // before a response was produced. …") and of the malformed / never-arrived variants of it ("… and no response was
    // produced. Try again.").
    inline constexpr std::wstring_view kCutOffResponseBeforeOutputText = L"before a response was produced";
    inline constexpr std::wstring_view kCutOffResponseNoOutputText = L"no response was produced";

    // At most this many automatic restarts of ONE session inside kConnectionLostRestartWindowMs for a CUT-OFF response (a
    // temporary failure has its own budget and window: kTransientRestartBudget, kTransientRestartWindowMs). A stream that keeps
    // getting cut off would otherwise restart the session forever; past the budget the session stays in Error for the
    // user, exactly as without this feature. Three covers a flaky stretch without hiding a dead network for long.
    inline constexpr size_t kConnectionLostRestartBudget = 3;

    // The rolling window of kConnectionLostRestartBudget (30 minutes). Long enough that a burst of drops is capped,
    // short enough that a session hit again hours later heals again.
    inline constexpr int64_t kConnectionLostRestartWindowMs = 30LL * 60 * 1000;

    // How long after an automatic restart the window waits to see the turn run again before it calls the restart a
    // failure (and shows the Error toast it held). It covers the restart's held start (up to kApiErrorRewindExitWaitMs,
    // 15 s, for the old claude to exit before the take-back line is written), a slow claude.exe start on a loaded PC
    // (MCP servers included) and the first request of the continuation. Too short would toast for a restart that is
    // still healing; too long only delays the toast of a restart that failed.
    inline constexpr int64_t kConnectionLostRestartConfirmMs = 120LL * 1000;

    // Does `text` contain the ASCII words `needle`, ignoring ASCII case? Pure; allocates nothing.
    //   text   — the text searched (any UTF-16; a non-ASCII character can never be part of a match, the needle being ASCII).
    //   needle — ASCII words (an empty needle matches nothing).
    // Returns true when the words are in the text. Never throws.
    inline bool ContainsAsciiWordsNoCase(std::wstring_view text, std::wstring_view needle) noexcept
    {
        if (needle.empty() || text.size() < needle.size())
        {
            return false;
        }
        const auto fold = [](wchar_t c) noexcept { return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c; };
        for (size_t start = 0; start + needle.size() <= text.size(); ++start)
        {
            size_t i = 0;
            while (i < needle.size() && fold(text[start + i]) == fold(needle[i]))
            {
                ++i;
            }
            if (i == needle.size())
            {
                return true;
            }
        }
        return false;
    }

    // Is this API error text one of Claude Code's CUT-OFF RESPONSES — the stream died after output began
    // (kCutOffResponseMidStreamText: "… The response above may be incomplete."; kCutOffResponseMidResponseText: "…
    // mid-response") or before any output (kCutOffResponseBeforeOutputText / kCutOffResponseNoOutputText: "… Try again.")? A
    // case-insensitive search for the
    // words anywhere in the text, so another prefix ("API Error: " today) or an added tail still matches. Only the text of an
    // API-error row is ever asked (SessionInfo::errorMessage is set from one; so is ApiErrorRewindPlan::errorText), so the
    // phrases cannot meet Claude Code's unrelated "may be incomplete" messages (a download, IDE detection). Pure; allocates
    // nothing.
    //   errorText — the error row's text as the scanner captured it (SessionInfo::errorMessage) or as the take-back planner
    //               read it (ApiErrorRewindPlan::errorText). Empty never matches.
    // Returns true for a cut-off response. Never throws.
    // ⚠ HTTP errors that outlived Claude Code's own retries ("API Error: 500 …", "529 Overloaded", "Rate limited"), usage
    //   limits and refusals are NOT cut-off responses and must never match: a restart of those repeats a request the server
    //   refused, or one the user's plan does not allow.
    inline bool IsCutOffResponseError(std::wstring_view errorText) noexcept
    {
        return ContainsAsciiWordsNoCase(errorText, kCutOffResponseMidStreamText) || ContainsAsciiWordsNoCase(errorText, kCutOffResponseMidResponseText) ||
               ContainsAsciiWordsNoCase(errorText, kCutOffResponseBeforeOutputText) || ContainsAsciiWordsNoCase(errorText, kCutOffResponseNoOutputText);
    }

    // ===== which API errors the automatic restart heals (2026-11-10: temporary failures beside the cut-off responses) =====

    // Where the ASCII words `needle` first occur in `text` at or after `from`, ignoring ASCII case — the position-returning twin
    // of ContainsAsciiWordsNoCase, for the readers that must look right behind the match (HttpStatusInErrorText). Pure;
    // allocates nothing.
    //   text   — the text searched (any UTF-16; a non-ASCII character can never be part of a match, the needle being ASCII).
    //   needle — ASCII words (an empty needle matches nothing).
    //   from   — the first position a match may start at (past the end finds nothing).
    // Returns the match's position, or std::wstring_view::npos when there is none. Never throws.
    inline size_t FindAsciiWordsNoCase(std::wstring_view text, std::wstring_view needle, size_t from = 0) noexcept
    {
        if (needle.empty() || from > text.size() || text.size() - from < needle.size())
        {
            return std::wstring_view::npos;
        }
        const auto fold = [](wchar_t c) noexcept { return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c; };
        for (size_t start = from; start + needle.size() <= text.size(); ++start)
        {
            size_t i = 0;
            while (i < needle.size() && fold(text[start + i]) == fold(needle[i]))
            {
                ++i;
            }
            if (i == needle.size())
            {
                return start;
            }
        }
        return std::wstring_view::npos;
    }

    // The words of every API error the automatic restart must NEVER heal, because only the user can: checked BEFORE every other
    // rule, so a text that also carries a "temporary" phrase ("Unable to connect to API: SSL certificate …") stays the user's.
    // Lower-case ASCII, matched case-insensitively anywhere in the text (ContainsAsciiWordsNoCase). Each line quotes the
    // Claude Code text (2.1.310's bundle, or a real transcript row on this PC) it is there for. Errors whose words match no list
    // are never restarted either: the lists only ever ADD a restart for words they know.
    inline constexpr std::wstring_view kAutoRestartNeverPhrases[] = {
        L"hit your", // "You've hit your limit · resets 2am", "You've hit your session limit · …", "… your monthly spend limit."
        L"extra usage", // "You're out of extra usage · resets …"
        L"usage credits", // "You're out of usage credits. Run /usage-credits …", "Usage credits are required for this model."
        L"spend limit", // "You've hit your monthly spend limit."
        L"credit balance", // "Credit balance is too low"
        L"billing", // a billing error ("billing_error")
        L"/login", // "Not logged in · Please run /login", "Please run /login · API Error: 401 Invalid authentication credentials"
        L"log in", // a sign-in only the user can do
        L"sign in", // "… sign out and sign in again for the plan to take effect."
        L"authentication", // "Invalid authentication credentials", "authentication_error"
        L"oauth", // "OAuth token has expired. Please obtain a new token …", "OAuth token has been revoked"
        L"api key", // "Invalid API key · Fix external API key"
        L"credentials", // "Could not load AWS credentials · … Check or refresh your … credentials and try again."
        L"verification", // "verification_required"
        L"/model", // "Opus is experiencing high load, please use /model to switch to Sonnet", "… Run /model to pick a different model."
        L"safeguards", // a refusal: "API Error: Opus 5.5's safeguards flagged this message (…)"
        L"content filtering", // "Output blocked by content filtering policy"
        L"prompt is too long", // the context is full ("Prompt is too long", "Prompt is too long · the request is ~… tokens")
        L"context window", // "API Error: The model has reached its context window limit."
        L"output token maximum", // "API Error: Claude's response exceeded the 32000 output token maximum. …"
        L"autocompact", // "Autocompact is thrashing: the context refilled to the limit …"
        L"certificate", // "API Error: Unable to connect to API: SSL certificate verification failed (…)" — a TLS set-up, not an outage
        L"ssl error", // "API Error: Unable to connect to API: SSL error (ERR_SSL_…)"
    };

    // The words of the TEMPORARY server-side and network failures Claude Code writes once its own retries gave up — restarted
    // after a back-off (TransientRestartBackoffMs). Lower-case ASCII, matched case-insensitively anywhere in the text, so another
    // prefix, a status in front, a JSON body around them or a changed tail still matches. Each line quotes the text it is there
    // for (2.1.310's bundle: its `${Kl}: …` templates and kbe()'s connection words; older versions' rows on this PC).
    inline constexpr std::wstring_view kTransientFailurePhrases[] = {
        L"server-side issue", // "API Error: 500 Internal server error. This is a server-side issue, usually temporary — try again in a moment."
        L"usually temporary", // "… The API is at capacity — this is usually temporary. …", "… Connection to the API was lost (EPIPE). This is usually temporary — try again."
        L"overloaded", // "API Error: Repeated 529 Overloaded errors. …", "API Error: 529 Overloaded. …", "API Error: Overloaded", "overloaded_error"
        L"internal server error", // "API Error: 500 {"type":"error","error":{"type":"api_error","message":"Internal server error"},…}" (older), "API Error: Internal server error"
        L"bad gateway", // "502 Bad Gateway"
        L"service unavailable", // "503 Service Unavailable"
        L"gateway timeout", // "504 Gateway Timeout"
        L"temporarily limiting requests", // "API Error: Server is temporarily limiting requests (not your usage limit) · Rate limited"
        L"not your usage limit", // the same 429, should its first words change
        L"temporary capacity issue", // "API Error: Request rejected (429) · this may be a temporary capacity issue."
        L"connection error", // "API Error: Connection error." (older Claude Code)
        L"connection dropped", // "API Error: Connection dropped (ECONNRESET)"
        L"connection lost", // "API Error: Connection lost", "… Connection lost while your computer was asleep" (a cut-off one is matched first)
        L"connection closed", // "API Error: Connection closed before the response finished"
        L"connection refused", // "API Error: Connection refused — a firewall or proxy may be blocking it (ECONNREFUSED)"
        L"connection to the api", // "API Error: Connection to the API was lost (ECONNRESET). This is usually temporary — try again."
        L"unable to connect to api", // "API Error: Unable to connect to API (ECONNREFUSED)" (a certificate problem is never restarted: checked first)
        L"reach the api server", // "API Error: Can't reach the API server — check your internet or DNS (ENOTFOUND)"
        L"no internet route", // "API Error: No internet route — check your connection or VPN (ENETUNREACH)"
        L"socket connection", // "API Error: The socket connection was closed unexpectedly. For more information, …" (older Claude Code)
        L"socket hang up", // Node's own words for a dropped socket
        L"request timed out", // "Request timed out", "API Error: Request timed out. Check your internet connection and proxy settings"
        L"no response from api", // "API Error: No response from API (waited 5m). If a proxy or gateway on your network holds responses …"
        L"stream idle timeout", // "API Error: Stream idle timeout - no chunks received" (2.1.220 – 2.1.291)
        L"empty response", // "The model returned an empty response (retry also failed)."
        L"tool call could not be parsed", // "The model's tool call could not be parsed (retry also failed)."
    };

    // What the automatic restart makes of an API error (ClassifyAutoRestartError).
    enum class AutoRestartErrorKind : uint8_t
    {
        // Not one it heals: the error needs the user (kAutoRestartNeverPhrases, a 4xx status), or its words are not known.
        None,
        // The response stream was cut off (IsCutOffResponseError): restarted AT ONCE.
        CutOff,
        // A server-side or network failure that outlived Claude Code's own retries (kTransientFailurePhrases, a 408 / 5xx
        // status): restarted after a back-off (TransientRestartBackoffMs).
        Transient,
    };

    // The words a log line names a kind by ("[auto-restart] … a cut-off response: …"). Pure; never throws.
    //   kind — the kind.
    // Returns a static string ("another error" for None and for a value outside the enum).
    inline const wchar_t* AutoRestartErrorKindWords(AutoRestartErrorKind kind) noexcept
    {
        switch (kind)
        {
        case AutoRestartErrorKind::CutOff:
            return L"a cut-off response";
        case AutoRestartErrorKind::Transient:
            return L"a temporary API failure";
        case AutoRestartErrorKind::None:
            break;
        }
        return L"another error";
    }

    // The HTTP status an error TEXT names right after "API Error: " ("API Error: 529 Overloaded. …", "API Error: 500
    // {"type":"error",…}", "Please run /login · API Error: 401 …") — exactly three ASCII digits not followed by a fourth. For
    // the rows whose apiErrorStatus is absent (older Claude Code wrote the status into the text only) and for the transcript
    // gate, whose plan carries no status. Pure; allocates nothing.
    //   text — the error row's text.
    // Returns the status (100–999), or 0 when the text names none. Never throws.
    inline int HttpStatusInErrorText(std::wstring_view text) noexcept
    {
        constexpr std::wstring_view kPrefix = L"API Error: ";
        const size_t at = FindAsciiWordsNoCase(text, kPrefix);
        if (at == std::wstring_view::npos)
        {
            return 0;
        }
        const size_t d = at + kPrefix.size();
        const auto isDigit = [](wchar_t c) noexcept { return c >= L'0' && c <= L'9'; };
        if (d + 3 > text.size() || !isDigit(text[d]) || !isDigit(text[d + 1]) || !isDigit(text[d + 2]) || (d + 3 < text.size() && isDigit(text[d + 3])) || text[d] == L'0')
        {
            return 0; // not a three-digit status ("API Error: Connection lost …", "API Error: 1234 …", "API Error: 042")
        }
        return (text[d] - L'0') * 100 + (text[d + 1] - L'0') * 10 + (text[d + 2] - L'0');
    }

    // Is this HTTP status a TEMPORARY failure whatever the words around it? 408 (the request timed out) and every 5xx (500
    // Internal Server Error, 502 / 503 / 504, 529 Overloaded) — the statuses Claude Code itself retries, 429 aside (a 429 is a
    // usage limit unless its words say otherwise: kTransientFailurePhrases). Pure; never throws.
    //   status — the status (0 = none).
    // Returns true for 408 and 500–599.
    inline bool IsTransientHttpStatus(int status) noexcept
    {
        return status == 408 || (status >= 500 && status <= 599);
    }

    // Is this HTTP status a request only the USER can fix? Every 4xx but 408 (a timeout) and 429 (a rate or usage limit, which
    // the phrases decide): a 400 bad request, a 401 / 403 sign-in, a 404 model, a 413 prompt too large — a restart would send
    // the same request again. Pure; never throws.
    //   status — the status (0 = none).
    // Returns true for 400–499 but 408 and 429.
    inline bool IsUserActionHttpStatus(int status) noexcept
    {
        return status >= 400 && status <= 499 && status != 408 && status != 429;
    }

    // WHICH API ERRORS THE AUTOMATIC RESTART HEALS. The rules, in order:
    //   1. an empty text, any kAutoRestartNeverPhrases words, or a user-action status (IsUserActionHttpStatus) -> None — the
    //      error needs the user, whatever else its text says;
    //   2. a cut-off response (IsCutOffResponseError) -> CutOff — restarted at once;
    //   3. any kTransientFailurePhrases words, or a temporary status (IsTransientHttpStatus) -> Transient — restarted after a
    //      back-off;
    //   4. anything else -> None (an unknown wording is never restarted).
    // The status is the row's own (`httpStatus`, from its apiErrorStatus) when given, else the one the text names
    // (HttpStatusInErrorText). Only the text of an API-error row is ever asked (SessionInfo::errorMessage is set from one; so
    // is ApiErrorRewindPlan::errorText). Pure; allocates nothing.
    //   errorText  — the error row's text (SessionInfo::errorMessage, or ApiErrorRewindPlan::errorText — its first line, at
    //                most 160 characters: every phrase above sits in the first sentence).
    //   httpStatus — the row's apiErrorStatus (SessionInfo::errorStatus), 0 when unknown or absent.
    // Returns the kind. Never throws.
    // ⚠ Rule 1 comes first on purpose: never move a "never" phrase below the cut-off or the temporary ones.
    inline AutoRestartErrorKind ClassifyAutoRestartError(std::wstring_view errorText, int httpStatus) noexcept
    {
        if (errorText.empty())
        {
            return AutoRestartErrorKind::None;
        }
        for (const auto phrase : kAutoRestartNeverPhrases)
        {
            if (ContainsAsciiWordsNoCase(errorText, phrase))
            {
                return AutoRestartErrorKind::None;
            }
        }
        const int status = httpStatus != 0 ? httpStatus : HttpStatusInErrorText(errorText);
        if (IsUserActionHttpStatus(status))
        {
            return AutoRestartErrorKind::None;
        }
        if (IsCutOffResponseError(errorText))
        {
            return AutoRestartErrorKind::CutOff;
        }
        for (const auto phrase : kTransientFailurePhrases)
        {
            if (ContainsAsciiWordsNoCase(errorText, phrase))
            {
                return AutoRestartErrorKind::Transient;
            }
        }
        return IsTransientHttpStatus(status) ? AutoRestartErrorKind::Transient : AutoRestartErrorKind::None;
    }

    // Is the registry's error text the same error row as the take-back planner's? The planner keeps the row's FIRST LINE,
    // trimmed, at most 160 characters with an ellipsis for a cut (ApiErrorRewindPlan::errorText); the registry keeps the whole
    // text (SessionInfo::errorMessage). The transcript gate counts the registry's HTTP status (SessionInfo::errorStatus) only for
    // that row — a newer error the registry has not seen yet must not borrow an older row's status. Pure; allocates nothing.
    //   registryText — SessionInfo::errorMessage.
    //   planText     — ApiErrorRewindPlan::errorText.
    // Returns true when the registry's text, past its leading white space, starts with the planner's line (its ellipsis
    // dropped); false when either is empty. Never throws.
    inline bool SameApiErrorText(std::wstring_view registryText, std::wstring_view planText) noexcept
    {
        while (!registryText.empty() && (registryText.front() == L' ' || registryText.front() == L'\t' || registryText.front() == L'\r' || registryText.front() == L'\n'))
        {
            registryText.remove_prefix(1);
        }
        if (!planText.empty() && planText.back() == L'\x2026')
        {
            planText.remove_suffix(1); // FirstLineCapped's mark of a line cut at 160 characters
        }
        return !planText.empty() && registryText.size() >= planText.size() && registryText.substr(0, planText.size()) == planText;
    }

    // The back-off before each automatic restart of a TEMPORARY failure, by the restarts already made within
    // kTransientRestartWindowMs: Claude Code's own /goal steps (1 minute, then 5, then 15). Claude Code's retries have just
    // spent minutes on the same failure, so an outage gets time to pass before the restart repeats the request.
    inline constexpr int64_t kTransientRestartBackoffMs[] = { 60LL * 1000, 5LL * 60 * 1000, 15LL * 60 * 1000 };

    // At most this many automatic restarts of ONE session for temporary failures within kTransientRestartWindowMs — one per
    // back-off step, as Claude Code's /goal gives up after its third retry. Past it the session stays in Error for the user.
    inline constexpr size_t kTransientRestartBudget = std::size(kTransientRestartBackoffMs);

    // The rolling window of kTransientRestartBudget (60 minutes): it must hold the whole back-off ladder (1 + 5 + 15 minutes)
    // plus Claude Code's own retries before each new error row (minutes each), or a server that stays down would see a fourth,
    // fifth, … restart instead of being left to the user.
    inline constexpr int64_t kTransientRestartWindowMs = 60LL * 60 * 1000;

    // The most a session's own share lengthens each back-off step (percent). A share fixed per session (a hash of its id, not
    // a random draw: the gate is pure and re-judged every tick) spreads a fleet hit by one outage over up to 12 s, 1 and 3
    // minutes instead of restarting every tab in the same second — Claude Code's own /goal jitters its steps by up to 20 %.
    inline constexpr int64_t kTransientRestartSpreadPercent = 20;

    // The wait before the next automatic restart of a session for a TEMPORARY failure. Pure; allocates nothing.
    //   restartsInWindow — the automatic restarts of this session within kTransientRestartWindowMs (the ledger's
    //                      restartsInTransientWindow): 0 -> the first step, 1 -> the second, 2 or more -> the last.
    //   sessionId        — the session (its FNV-1a hash picks its fixed 0–kTransientRestartSpreadPercent % share).
    // Returns the back-off in milliseconds (60–72 s, 5–6 min or 15–18 min). Never throws.
    inline int64_t TransientRestartBackoffMs(size_t restartsInWindow, std::wstring_view sessionId) noexcept
    {
        const size_t step = restartsInWindow < kTransientRestartBudget ? restartsInWindow : kTransientRestartBudget - 1;
        const int64_t base = kTransientRestartBackoffMs[step];
        uint32_t hash = 2166136261u; // FNV-1a over the id's UTF-16 units: the same session always gets the same share
        for (const wchar_t c : sessionId)
        {
            hash ^= static_cast<uint32_t>(c);
            hash *= 16777619u;
        }
        const int64_t percent = static_cast<int64_t>(hash % static_cast<uint32_t>(kTransientRestartSpreadPercent + 1));
        return base + base * percent / 100;
    }

    // What the registry gate decided for one session at one moment. The order is the order of the checks: a session that
    // is not this feature's business is Ignore (nothing logged); an error it heals but Agentmaster will not restart is Decline
    // (logged once per Error episode); one it will restart once something ends is Hold; a temporary failure whose back-off
    // has not passed is Wait; Go starts the transcript gate.
    enum class ConnectionLostRestartGate
    {
        // Not this feature's business: not a live Claude session hosted in this window, not in an unacknowledged Error,
        // an error the automatic restart does not heal (ClassifyAutoRestartError: None), or a dormant tab (its own start takes
        // the error back — the restored resume and Deactivate's wake do — and the shipped CLAUDE_CODE_RESUME_INTERRUPTED_TURN=1
        // continues it).
        Ignore,
        // An error it heals that this window will not restart: the switch is off, an adopted tab, the take-back switch is off,
        // a stop intent the user recorded. The session stays in Error for the user, as without the feature.
        Decline,
        // An error it heals that this window WILL restart once the reason passes (an unsent draft, a delivery or a draft fill
        // owning the input box, background work a restart would end). Re-evaluated on every registry change of the session
        // and on every liveness tick.
        Hold,
        // A TEMPORARY failure (AutoRestartErrorKind::Transient) whose back-off (ConnectionLostRestartFacts::backoffMs) has not
        // passed since the Error episode began: restarted once it has. Re-evaluated on every liveness tick, like a hold; unlike
        // a hold, the Running -> Error toast stays held meanwhile (the restart may still heal the session).
        Wait,
        // Restart it — after the transcript gate (DecideConnectionLostRestartFromPlan) agrees.
        Go,
    };

    // The registry facts the gate reads, gathered by the window from the session's record and its own state. Plain
    // values, so a test can state every case.
    struct ConnectionLostRestartFacts
    {
        bool settingOn{ false }; // AppSettings::autoRestartOnConnectionLost — the feature's switch
        bool takeBackOn{ false }; // AppSettings::resumeApiErrorTurns — without it a restart never takes the error back, so it would heal nothing
        bool isClaude{ false }; // SessionInfo::kind == AgentKind::Claude (a Codex rollout has no such rows)
        bool live{ false }; // SessionInfo::live (an archived record has no tab to restart)
        bool hostedHere{ false }; // this window's _claudeTabs holds the session's tab (only the hosting window may restart its ConPTY)
        bool started{ false }; // SessionInfo::started — its claude ran in this tab (false == a dormant tab)
        bool external{ false }; // SessionInfo::external — an ADOPTED session: its ConPTY is the user's own shell
        bool stopIntent{ false }; // SessionInfo::stopForkedTurn — the user chose that this session's continued turn be STOPPED ("Fork interrupted" / "Restart interrupted"); a LetTurnContinue restart would clear that choice
        SessionState state{ SessionState::Idle }; // SessionInfo::state
        bool errorDismissed{ false }; // SessionInfo::errorDismissed — the user acknowledged this error ("Move to Idle/Done")
        std::wstring_view errorMessage; // SessionInfo::errorMessage — the error row's text the scanner captured
        int errorStatus{ 0 }; // SessionInfo::errorStatus — the error row's apiErrorStatus (0 = none): a 408 / 5xx is a temporary failure, a 4xx but 408 / 429 needs the user (ClassifyAutoRestartError)
        bool draftInBox{ false }; // SessionInfo::pendingInput is not empty — an unsent draft a restart would wipe
        bool boxBusy{ false }; // a delivery, a draft swap, a draft type-back or the fork stop watch owns the input box
        bool backgroundWork{ false }; // claude's heartbeat says busy / shell, or its side files are fresh (agents, shells)
        int64_t errorAgeMs{ 0 }; // how long this Error episode has lasted (wall clock: now − SessionInfo::stateChangedAtUnixMs); filled for a temporary failure only
        int64_t backoffMs{ 0 }; // the wait before a temporary failure's restart (TransientRestartBackoffMs of the ledger's count); 0 for a cut-off response, which restarts at once
    };

    // The registry gate's answer: the verdict and, for every verdict but Go, why — a static string for the log.
    struct ConnectionLostRestartRuling
    {
        ConnectionLostRestartGate gate{ ConnectionLostRestartGate::Ignore };
        std::wstring_view reason; // a string literal (static storage), never a view into the facts
    };

    // THE REGISTRY GATE: should the window restart this session now, later, or never for this Error episode? Pure and
    // cheap (no I/O), so the window runs it on every registry change of every session it hosts.
    //   f — the facts (ConnectionLostRestartFacts).
    // Returns the ruling; its reason is a string literal. Never throws.
    // ⚠ The checks are ordered: Ignore before Decline before Hold before Wait before Go. A Decline must stay a Decline
    //   whatever holds (a draft must not turn "the switch is off" into "waiting for the draft"), and a session that is not
    //   in an error the restart heals must read Ignore whatever else is true, or the window would log a line for every
    //   session it hosts. The back-off comes last: it is the restart's own pacing, never a reason to skip a decline or a hold.
    inline ConnectionLostRestartRuling DecideConnectionLostRestart(const ConnectionLostRestartFacts& f) noexcept
    {
        if (!f.isClaude || !f.live || !f.hostedHere)
        {
            return { ConnectionLostRestartGate::Ignore, L"not a live Claude session hosted in this window" };
        }
        if (f.state != SessionState::Error || f.errorDismissed)
        {
            return { ConnectionLostRestartGate::Ignore, L"not in an unacknowledged Error" };
        }
        // Which error it is decides everything below: a cut-off response restarts at once, a temporary failure after its
        // back-off, anything else (an error that needs the user, an unknown wording) never.
        const AutoRestartErrorKind kind = ClassifyAutoRestartError(f.errorMessage, f.errorStatus);
        if (kind == AutoRestartErrorKind::None)
        {
            return { ConnectionLostRestartGate::Ignore, L"an error the automatic restart does not heal (it needs you, or its words are unknown)" };
        }
        if (!f.started)
        {
            // A dormant tab heals at its own start: the restore and the Deactivate seams take the error back before the
            // resumed claude reads the file, and the shipped CLAUDE_CODE_RESUME_INTERRUPTED_TURN=1 continues it. Restarting
            // it would only start a claude nobody looks at (and _RestartPaneWithTurnChoice refuses a never-started control).
            return { ConnectionLostRestartGate::Ignore, L"a dormant tab (its own start takes the error back)" };
        }
        if (!f.settingOn)
        {
            return { ConnectionLostRestartGate::Decline, L"the automatic restart is off (Settings > Sessions)" };
        }
        if (f.external)
        {
            // An adopted session's ConPTY is the shell the user typed `claude` into: a restart replays that shell's
            // profile, it does not resume the conversation (the Deactivate seam refuses these tabs for the same reason).
            return { ConnectionLostRestartGate::Decline, L"an adopted session (its tab hosts the user's own shell, which a restart would replace)" };
        }
        if (!f.takeBackOn)
        {
            return { ConnectionLostRestartGate::Decline, L"\"Continue a turn that ended in an API error\" is off (a restart would wait at the error)" };
        }
        if (f.stopIntent)
        {
            // "Fork interrupted" / "Restart interrupted" recorded that the next continuation of this session must be stopped:
            // "Restart uninterrupted" is exactly the answer the user did not give (it clears that intent and lets the turn run).
            return { ConnectionLostRestartGate::Decline, L"the user chose to keep this session's turn stopped (\"Fork interrupted\" / \"Restart interrupted\")" };
        }
        if (f.draftInBox)
        {
            return { ConnectionLostRestartGate::Hold, L"an unsent draft is in the input box (a restart would wipe it)" };
        }
        if (f.boxBusy)
        {
            return { ConnectionLostRestartGate::Hold, L"a delivery, a draft fill or the fork stop watch owns the input box" };
        }
        if (f.backgroundWork)
        {
            return { ConnectionLostRestartGate::Hold, L"claude still runs background work (a shell or an agent) that a restart would end" };
        }
        if (kind == AutoRestartErrorKind::Transient && f.errorAgeMs < f.backoffMs)
        {
            // Claude Code's own retries have just spent minutes on this failure: an outage gets its back-off to pass before
            // the restart sends the request again (the window logs how long is left).
            return { ConnectionLostRestartGate::Wait, L"the back-off after a temporary API failure (Claude Code's own retries have just given up)" };
        }
        return { ConnectionLostRestartGate::Go, L"" };
    }

    // What the transcript gate decided once the take-back planner read the session's tail.
    enum class ConnectionLostPlanVerdict
    {
        // Restart it (RestartTurnChoice::LetTurnContinue).
        Restart,
        // The active branch no longer ends in an API error (the planner's NotApplicable): the user rewound or retried
        // meanwhile, or the error was taken back already. Nothing to heal.
        NothingToTakeBack,
        // The planner left the error alone (a refusal, an unknown shape, a torn last line, a file larger than it reads):
        // a restart would take nothing back and the new claude would wait at the error.
        NotTakenBack,
        // The transcript's active error is not one the automatic restart heals (ClassifyAutoRestartError: None — the
        // registry's message was stale).
        NotThisError,
        // The take-back lands on a row Claude Code does not continue from by itself (a meta row, a compaction summary).
        WouldNotContinue,
        // This very error row was restarted for already: that restart did not take it back (another tab's claude runs the
        // conversation, the old claude outlived the 15 s wait), so a second restart would fail the same way.
        AlreadyRestarted,
        // kConnectionLostRestartBudget automatic restarts of this session within kConnectionLostRestartWindowMs (a cut-off
        // response), or kTransientRestartBudget within kTransientRestartWindowMs (a temporary failure).
        OverBudget,
    };

    // The log word of a transcript-gate verdict ("restart", "nothing-to-take-back", …). Pure; never throws.
    //   v — the verdict.
    // Returns a static string ("unknown" for a value outside the enum — a verdict cast from a stray integer).
    inline const wchar_t* ConnectionLostPlanVerdictName(ConnectionLostPlanVerdict v) noexcept
    {
        switch (v)
        {
        case ConnectionLostPlanVerdict::Restart:
            return L"restart";
        case ConnectionLostPlanVerdict::NothingToTakeBack:
            return L"nothing-to-take-back";
        case ConnectionLostPlanVerdict::NotTakenBack:
            return L"not-taken-back";
        case ConnectionLostPlanVerdict::NotThisError:
            return L"not-this-error";
        case ConnectionLostPlanVerdict::WouldNotContinue:
            return L"would-not-continue";
        case ConnectionLostPlanVerdict::AlreadyRestarted:
            return L"already-restarted";
        case ConnectionLostPlanVerdict::OverBudget:
            return L"over-budget";
        }
        return L"unknown";
    }

    // The transcript gate's answer: the verdict and a sentence for the log (empty for Restart).
    struct ConnectionLostPlanRuling
    {
        ConnectionLostPlanVerdict verdict{ ConnectionLostPlanVerdict::NothingToTakeBack };
        std::wstring reason;
    };

    // What the process remembers of one session's automatic restarts (ConnectionLostRestartLedger::Look, whose Entry this is):
    // the transcript gate's "never twice for one row" and its budgets, and the back-off step of a temporary failure.
    struct AutoRestartHistory
    {
        std::wstring lastErrorUuid; // the error row of the newest automatic restart ("" when none)
        size_t restartsInWindow{ 0 }; // automatic restarts (of any kind) within kConnectionLostRestartWindowMs of the asked time — the cut-off budget
        size_t restartsInTransientWindow{ 0 }; // automatic restarts (of any kind) within kTransientRestartWindowMs — the temporary-failure budget and back-off step
    };

    // THE TRANSCRIPT GATE: once PlanApiErrorRewindForSession has read the session's tail, may the window restart it? The
    // restart must HEAL — the planner must take this error back onto a row Claude Code continues from, and the error must be
    // one the automatic restart heals (ClassifyAutoRestartError) — and it must not loop: never twice for one error row, never
    // more than the error kind's budget in its window (kConnectionLostRestartBudget in kConnectionLostRestartWindowMs for a
    // cut-off response, kTransientRestartBudget in kTransientRestartWindowMs for a temporary failure). Restarts of either kind
    // count against both budgets: a session whose stream keeps getting cut and whose requests keep failing is one sick
    // session. Pure.
    //   plan        — the take-back plan of the session's transcript (ApiErrorRewind.h).
    //   errorStatus — the error row's HTTP status as the registry knows it (SessionInfo::errorStatus), 0 when unknown — pass
    //                 it only when the registry's text is the plan's row (SameApiErrorText).
    //   history     — what the ledger remembers of this session (ConnectionLostRestartLedger::Look).
    // Returns the ruling.
    // Throws std::bad_alloc only, when the reason sentence cannot be allocated (the caller's catch logs it, Rule #18).
    inline ConnectionLostPlanRuling DecideConnectionLostRestartFromPlan(const ApiErrorRewindPlan& plan, int errorStatus, const AutoRestartHistory& history)
    {
        if (plan.verdict == ApiErrorRewindVerdict::NotApplicable)
        {
            return { ConnectionLostPlanVerdict::NothingToTakeBack, L"the transcript's active branch no longer ends in an API error" };
        }
        if (plan.verdict != ApiErrorRewindVerdict::Rewind)
        {
            return { ConnectionLostPlanVerdict::NotTakenBack, L"the take-back planner leaves this error alone: " + (plan.reason.empty() ? std::wstring{ L"(no reason given)" } : plan.reason) };
        }
        const AutoRestartErrorKind kind = ClassifyAutoRestartError(plan.errorText, errorStatus);
        if (kind == AutoRestartErrorKind::None)
        {
            return { ConnectionLostPlanVerdict::NotThisError, L"the transcript's error is \"" + plan.errorText + L"\", not one the automatic restart heals" };
        }
        if (!ApiErrorTakeBackContinues(plan))
        {
            return { ConnectionLostPlanVerdict::WouldNotContinue, L"the take-back lands on a " + plan.targetKind + L" row, which Claude Code does not continue by itself" };
        }
        if (!plan.errorUuid.empty() && plan.errorUuid == history.lastErrorUuid)
        {
            return { ConnectionLostPlanVerdict::AlreadyRestarted, L"this error row was restarted for already, and that restart did not take it back" };
        }
        if (kind == AutoRestartErrorKind::CutOff && history.restartsInWindow >= kConnectionLostRestartBudget)
        {
            return { ConnectionLostPlanVerdict::OverBudget,
                     std::to_wstring(history.restartsInWindow) + L" automatic restarts of this session in the last " + std::to_wstring(kConnectionLostRestartWindowMs / 60000) + L" minutes" };
        }
        if (kind == AutoRestartErrorKind::Transient && history.restartsInTransientWindow >= kTransientRestartBudget)
        {
            return { ConnectionLostPlanVerdict::OverBudget, std::to_wstring(history.restartsInTransientWindow) + L" automatic restarts of this session in the last " +
                                                                std::to_wstring(kTransientRestartWindowMs / 60000) + L" minutes, and the API keeps failing" };
        }
        return { ConnectionLostPlanVerdict::Restart, std::wstring{} };
    }

    // The transcript gate with the registry's status unknown and one restart count for both windows — the cut-off response's
    // original call shape (2026-11-08), which the tests of that family keep using. Equal to
    // DecideConnectionLostRestartFromPlan(plan, 0, { lastRestartedErrorUuid, restartsInWindow, restartsInWindow }). Pure.
    //   plan                    — the take-back plan of the session's transcript (ApiErrorRewind.h).
    //   lastRestartedErrorUuid  — the error row the previous automatic restart of this session was made for ("" when none).
    //   restartsInWindow        — the automatic restarts of this session, counted against both budgets.
    // Returns the ruling.
    // Throws std::bad_alloc only, when the history or the reason sentence cannot be allocated (the caller's catch logs it).
    inline ConnectionLostPlanRuling DecideConnectionLostRestartFromPlan(const ApiErrorRewindPlan& plan, std::wstring_view lastRestartedErrorUuid, size_t restartsInWindow)
    {
        AutoRestartHistory history;
        history.lastErrorUuid.assign(lastRestartedErrorUuid);
        history.restartsInWindow = restartsInWindow;
        history.restartsInTransientWindow = restartsInWindow;
        return DecideConnectionLostRestartFromPlan(plan, 0, history);
    }

    // What became of a session the window restarted automatically.
    enum class ConnectionLostRestartOutcome
    {
        // Not decided yet: the restart's held start, the take-back line, the claude's start and its first request take a
        // few seconds, and an Error the scanner asserts again meanwhile (the error stays the active leaf until the line is
        // written) is part of that, not a failure.
        Waiting,
        // The turn went on: it runs again (Running, or NeedsApproval — the continuation asked for a permission), or a turn
        // ENDED after the restart with the session out of Error (a continuation answered so fast that no scan saw it run —
        // measured with the e2e mock, flows/06 INT-01).
        Healed,
        // The restart did not heal it: kConnectionLostRestartConfirmMs passed with no turn going on, or a turn ended after
        // the restart with the session in Error again (the continuation itself failed).
        NotHealed,
        // The session left the fleet (its tab closed): nothing to watch.
        Gone,
    };

    // AFTER THE RESTART: did it heal the session? Pure.
    //   live                  — SessionInfo::live now.
    //   state                 — SessionInfo::state now.
    //   turnEndedSinceRestart — SessionInfo::turnEndUnixMs is newer than the restart (wall clock). Only a REAL Stop hook or a
    //                           transcript line with a terminal stop_reason stamps it — never a synthesized Stop — and the
    //                           old claude's hooks are dropped once the restart retired its host token, so a newer stamp is
    //                           the new claude finishing a turn: the continuation.
    //   sinceRestartMs        — milliseconds since the restart ran (a monotonic clock; negative reads as 0).
    // Returns the outcome. Never throws.
    inline ConnectionLostRestartOutcome DecideConnectionLostRestartOutcome(bool live, SessionState state, bool turnEndedSinceRestart, int64_t sinceRestartMs) noexcept
    {
        if (!live)
        {
            return ConnectionLostRestartOutcome::Gone;
        }
        if (state == SessionState::Running || state == SessionState::NeedsApproval)
        {
            return ConnectionLostRestartOutcome::Healed;
        }
        if (turnEndedSinceRestart)
        {
            // The continuation ran and ended. In Error it died again (an Error before that end is the held start's, which
            // ends no turn); anywhere else it finished, too fast for a scan to have seen it run.
            return state == SessionState::Error ? ConnectionLostRestartOutcome::NotHealed : ConnectionLostRestartOutcome::Healed;
        }
        if (sinceRestartMs >= kConnectionLostRestartConfirmMs)
        {
            return ConnectionLostRestartOutcome::NotHealed;
        }
        return ConnectionLostRestartOutcome::Waiting;
    }

    // Should a session the automatic restart HEALED get back the Prompt Queue Runner mode that the stop-on-error backstop
    // took away when the cut-off response put it in Error? Only when the record of that pause belongs to the very Error
    // episode that was healed, nothing changed the mode since (it is still Off), and the pause took a real mode away (an
    // interrupt hold's Off is not given back). Pure.
    //   a                — the session's runner state now (AutorunnerState::stopOnErrorPausedMode / …EpisodeMs are set by
    //                      Scheduler::OnObserved and forgotten by ClearInterruptHold — every later mode decision).
    //   healedEpisodeMs  — SessionInfo::stateChangedAtUnixMs of the Error episode the automatic restart was made for.
    // Returns true when the runner should go back to a.stopOnErrorPausedMode. Never throws.
    inline bool ShouldGiveRunnerBackAfterRestart(const AutorunnerState& a, int64_t healedEpisodeMs) noexcept
    {
        return a.mode == AutorunnerMode::Off && a.stopOnErrorPausedMode != AutorunnerMode::Off && a.stopOnErrorPausedEpisodeMs != 0 &&
               a.stopOnErrorPausedEpisodeMs == healedEpisodeMs;
    }

    // The process-wide memory of the automatic restarts: per session, the error row the last one was made for and when each
    // one ran. One instance per process (SharedConnectionLostRestartLedger), because one engine serves every window (Rule
    // #10) and a tab torn out into another window must keep its budget. Thread-safe (every window's UI thread uses it).
    // In memory only: an app restart starts every budget afresh, which is fine — the app's own restart already resumed
    // every tab once.
    class ConnectionLostRestartLedger
    {
    public:
        // What the ledger knows about one session (AutoRestartHistory: the newest restart's error row and the restarts within
        // each budget's window).
        using Entry = AutoRestartHistory;

        // Look a session up.
        //   sessionId — the session.
        //   nowMs     — the caller's monotonic clock (the same clock as Note's).
        // Returns its entry (empty for a session never restarted): the restarts of either kind within the cut-off window
        // (kConnectionLostRestartWindowMs) and within the temporary-failure window (kTransientRestartWindowMs).
        // Throws std::system_error when the mutex cannot be locked, std::bad_alloc when the copy cannot be allocated.
        Entry Look(std::wstring_view sessionId, int64_t nowMs) const
        {
            std::lock_guard lock{ _mtx };
            Entry out;
            const auto it = _sessions.find(sessionId);
            if (it == _sessions.end())
            {
                return out;
            }
            out.lastErrorUuid = it->second.lastErrorUuid;
            for (const int64_t t : it->second.restartsMs)
            {
                if (_InWindow(t, nowMs))
                {
                    ++out.restartsInWindow;
                }
                if (_InWindowOf(t, nowMs, kTransientRestartWindowMs))
                {
                    ++out.restartsInTransientWindow;
                }
            }
            return out;
        }

        // Record an automatic restart, BEFORE it runs (so nothing that runs inside it can start a second one).
        //   sessionId — the session restarted.
        //   errorUuid — the error row it is made for (ApiErrorRewindPlan::errorUuid; "" when unknown).
        //   nowMs     — the caller's monotonic clock.
        // Throws std::system_error when the mutex cannot be locked, std::bad_alloc when the record cannot grow.
        void Note(std::wstring_view sessionId, std::wstring_view errorUuid, int64_t nowMs)
        {
            std::lock_guard lock{ _mtx };
            auto it = _sessions.find(sessionId);
            if (it == _sessions.end())
            {
                it = _sessions.emplace(std::wstring{ sessionId }, Record{}).first;
            }
            auto& times = it->second.restartsMs;
            // Keep only the restarts that still count in the LONGEST window (a shorter one would drop restarts the
            // temporary-failure budget still counts), so a session restarted every few hours never grows its record.
            times.erase(std::remove_if(times.begin(), times.end(), [nowMs](int64_t t) { return !_InWindowOf(t, nowMs, _KeepMs()); }), times.end());
            times.push_back(nowMs);
            it->second.lastErrorUuid.assign(errorUuid);
        }

    private:
        // One session's history.
        struct Record
        {
            std::wstring lastErrorUuid;
            std::vector<int64_t> restartsMs;
        };

        // Does a restart at `t` still count at `nowMs` for the cut-off budget (kConnectionLostRestartWindowMs)? A time in the
        // future (a clock that went back) counts, so a clock jump can only make the budget stricter, never looser.
        static bool _InWindow(int64_t t, int64_t nowMs) noexcept
        {
            return nowMs - t < kConnectionLostRestartWindowMs;
        }

        // _InWindow for any window: does a restart at `t` still count at `nowMs` within `windowMs`? The same clock rule (a
        // future time counts).
        static bool _InWindowOf(int64_t t, int64_t nowMs, int64_t windowMs) noexcept
        {
            return nowMs - t < windowMs;
        }

        // How long a restart is kept at all: the longest window any budget counts in.
        static constexpr int64_t _KeepMs() noexcept
        {
            return kConnectionLostRestartWindowMs > kTransientRestartWindowMs ? kConnectionLostRestartWindowMs : kTransientRestartWindowMs;
        }

        mutable std::mutex _mtx;
        std::map<std::wstring, Record, std::less<>> _sessions; // std::less<> — Look / Note take a wstring_view without a copy
    };

    // The one ledger of this process (a function-local static: built on first use, thread-safe by the language).
    // Returns the ledger. Never throws.
    inline ConnectionLostRestartLedger& SharedConnectionLostRestartLedger() noexcept
    {
        static ConnectionLostRestartLedger ledger;
        return ledger;
    }
}
