// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the session database's TOOL REGISTRY (SESSIONS_DB.md §8.4): one entry per tool — its
// name, the description the model reads, its JSON Schema, its budget and its handler. The text (name,
// description, schema, budget) lives in ../SessionDbToolDefs.h, the one copy the app reads too; this
// registry pairs each definition with its handler. Every consumer builds its tool list from there, so
// they cannot disagree (§8.4.4 rule 1):
//   - the stdio MCP server (`agentmaster-sessiondb.exe mcp`, server name `agentmaster`): tools/list
//     answers ToolDefinitionsJson(), tools/call reaches CallTool through the service's `toolCall`;
//   - the service's protocol methods (`search`, `sessionDetail`, `messages`, ...): each one is the
//     tool of the same contract;
//   - the in-app AI search with the session database On (SessionSearchTools.cpp): the same tools as the
//     in-process server `agentmaster`, each call forwarded through the pipe's `toolCall`.
//
// The conventions of §8.4.2 hold for every tool: read-only; sessions named by short ids (8+ characters
// of the UUID, longer when two share them), blocks by refs (`<id>:<line>.<block>`, a subagent's block
// `<id>/<agent>:<line>.<block>`); local times `YYYY-MM-DD HH:MM` with `now` in every result; compact
// JSON of at most 64 KB with long text cut and marked `truncated`; paging by an opaque `next` cursor that
// is valid only with the same other parameters; `partial` when a budget cut the work short; errors that
// say what is wrong and how to fix the call; ASCII descriptions.
//
// `show` is NOT here: it changes the Agentmaster window, so it lives in the app (the in-app harness), and
// another Claude session never gets it (§8.4.3 `show`).

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace Agentmaster::SessionDb
{
    class Connection;
    class Store;
    class SearchEngine;

    // What a handler gets for one call.
    struct ToolContext
    {
        Store& store;
        SearchEngine& search;
        Connection& con; // a reader connection for this call (the service's pool); queries may be interrupted
        std::function<bool()> cancel; // true = the caller gave up (cancel / timeout); stop at the next step
        int64_t nowMs{ 0 }; // the request's clock (Unix ms), so one result names one `now`
    };

    // A user error inside a handler: the message is shown to the model (what is wrong, how to fix it).
    // `code` is the protocol's error code (SessionDbProtocol::kErr*), for the pipe methods.
    struct ToolError
    {
        int code;
        std::string message;
    };

    // One registry entry.
    struct ToolSpec
    {
        std::string name; // e.g. "search" (the MCP name is mcp__agentmaster__<name>)
        std::string description; // the exact text the model reads (§8.4.3)
        std::string inputSchema; // JSON Schema text (additionalProperties false, bounds, defaults)
        int64_t budgetMs{ 0 }; // the work budget; the pipe adds a margin before it interrupts the query
        // The handler: returns the result object, or throws ToolError for a user error. Any other
        // exception is an internal error (CallTool logs it and answers a generic message).
        std::function<nlohmann::json(ToolContext&, const nlohmann::json& args)> handler;
    };

    // The registry, in the order tools/list presents it (SessionDbToolDefs::kTools' order). Built at the first
    // call. Throws std::logic_error when a definition has no handler (a build mistake; every call throws again).
    const std::vector<ToolSpec>& ToolRegistry();

    // The tool list as MCP tool definitions: [{name, description, inputSchema, annotations}] (§8.4.2:
    // readOnlyHint / idempotentHint true, openWorldHint false).
    nlohmann::json ToolDefinitionsJson();

    // The outcome of one call, as MCP's tools/call result carries it.
    struct ToolOutcome
    {
        bool isError{ false };
        int code{ 0 }; // the protocol error code when isError (0 otherwise)
        std::string text; // the result JSON (compact) or the error message
        nlohmann::json result; // the result object (null when isError)
    };

    // Run one tool by name. Never throws: a ToolError becomes an error outcome with its message; an
    // interrupted query becomes "cancelled" / "timed out"; anything else is logged and answered with a
    // generic internal-error message.
    ToolOutcome CallTool(ToolContext& ctx, std::string_view name, const nlohmann::json& args);

    // ===== helpers the service's protocol methods share with the tools =====

    // Local time "YYYY-MM-DD HH:MM" of a Unix ms instant ("" for 0).
    std::string LocalTime(int64_t unixMs);
    // Parse a local date "YYYY-MM-DD" to the Unix ms of its local midnight (endOfDay: the last ms of that
    // day). Returns false for any other form.
    bool ParseLocalDate(std::string_view text, bool endOfDay, int64_t& unixMs);
    // UTF-16 <-> UTF-8 for JSON fields (invalid sequences become U+FFFD; never throws beyond bad_alloc).
    std::string Utf8(std::wstring_view text);
    std::wstring Wide(std::string_view utf8);
}
