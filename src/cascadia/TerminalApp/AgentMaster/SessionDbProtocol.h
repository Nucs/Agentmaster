// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the session database's WIRE CONTRACT (SESSIONS_DB.md §4.1, §9), shared by the
// service (agentmaster-sessiondb.exe) and every client (the GUI's refresh tick and Sessions page,
// the agentmaster CLI, the stdio MCP server).
//
// What lives here, and why in one header:
//   - the NAMES every process must agree on without talking first: the per-profile pipe, the
//     single-instance mutex, the files under <profile>\sessiondb\ — derived from the profile path
//     the same way in every process, so a CLI finds the service the GUI started;
//   - the FRAMING (a 4-byte little-endian length, then UTF-8 JSON) and its size limits;
//   - the version numbers a `hello` exchanges (§4.2 "Versions": a newer client asks an older
//     service to shut down; an older client never replaces a newer service);
//   - the method names (one per request type, §9).
//
// PURE and header-only: no <windows.h>, no DuckDB, no JSON library — so the m5 harness tests it
// and TerminalApp includes it without pulling anything heavy into its translation units.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "Sha256.h"

namespace Agentmaster::SessionDbProtocol
{
    // ===== versions ==============================================================================

    // The service build. Bumped with any behavior change a client could notice; reported by `hello`.
    // A client that is NEWER than the running service (compares kServiceBuild) asks it to `shutdown`
    // and starts its own copy (§4.2) — so the GUI's copied-out binary always ends up serving.
    inline constexpr int kServiceBuild = 1;
    // The database schema (SessionDbSchema.cpp). A service refuses a file whose schema is NEWER than
    // its own (never writes it); an older file is migrated or re-derived from the stored lines (§11).
    inline constexpr int kSchemaVersion = 1;
    // The tool registry (SessionDbTools.cpp, §8.4.4 rule 8): bumped whenever a tool's name,
    // description or schema changes, since that changes the model's prompt (and its cache).
    inline constexpr int kToolRegistryVersion = 1;
    // The framing + envelope shape on the pipe. A mismatch is refused at `hello`.
    inline constexpr int kProtocolVersion = 1;

    // ===== limits (§9) ===========================================================================

    // A request frame larger than this is refused (and the connection dropped): a well-formed client
    // never sends one (the largest request is an upsertSessions naming a few hundred paths).
    inline constexpr uint32_t kMaxRequestBytes = 1u * 1024u * 1024u;
    // A response is cut at this size by the service (the tools cap their own results far below it;
    // `sql` and `rebuild` are the ones that could approach it).
    inline constexpr uint32_t kMaxResponseBytes = 16u * 1024u * 1024u;

    // How long a service keeps running with no client connected (§4.2): 10 minutes.
    inline constexpr int64_t kIdleExitMs = 10 * 60 * 1000;

    // ===== method names (§9) =====================================================================

    inline constexpr std::string_view kHello = "hello";
    inline constexpr std::string_view kStatus = "status";
    inline constexpr std::string_view kShutdown = "shutdown";
    inline constexpr std::string_view kUpsertSessions = "upsertSessions";
    inline constexpr std::string_view kResync = "resync";
    inline constexpr std::string_view kFactsChanged = "factsChanged";
    inline constexpr std::string_view kOpenChanged = "openChanged";
    inline constexpr std::string_view kListSessions = "listSessions";
    inline constexpr std::string_view kSearch = "search";
    inline constexpr std::string_view kSessionDetail = "sessionDetail";
    inline constexpr std::string_view kSessionSummary = "sessionSummary";
    inline constexpr std::string_view kMessages = "messages";
    inline constexpr std::string_view kGrep = "grep";
    inline constexpr std::string_view kFiles = "files";
    inline constexpr std::string_view kWhoTouched = "whoTouched";
    inline constexpr std::string_view kTimeline = "timeline";
    inline constexpr std::string_view kUsage = "usage";
    inline constexpr std::string_view kRelated = "related";
    inline constexpr std::string_view kSql = "sql";
    inline constexpr std::string_view kRebuild = "rebuild";
    inline constexpr std::string_view kToolList = "toolList"; // the registry as MCP tool definitions
    inline constexpr std::string_view kToolCall = "toolCall"; // run one registry tool by name (MCP, CLI)
    inline constexpr std::string_view kCancel = "cancel";

    // ===== error codes (the `error.code` of a response) ==========================================

    inline constexpr int kErrBadRequest = 400; // malformed JSON, unknown method, a bad parameter
    inline constexpr int kErrNotFound = 404; // an unknown session id / ref
    inline constexpr int kErrConflict = 409; // an ambiguous id prefix
    inline constexpr int kErrTimeout = 408; // the request's budget ran out (its query was interrupted)
    inline constexpr int kErrCancelled = 499; // a `cancel` (or a newer request) interrupted it
    inline constexpr int kErrInternal = 500; // anything else (the service logged it)
    inline constexpr int kErrUnavailable = 503; // the database is not open (a rebuild after a broken file)

    // ===== names derived from the profile ========================================================

    // UTF-16 -> UTF-8 for the hash input (pure; a lone surrogate encodes as U+FFFD so every process
    // hashes the same bytes for the same path). Never throws beyond std::bad_alloc.
    inline std::string Utf16ToUtf8ForHash(std::wstring_view s)
    {
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i)
        {
            uint32_t cp = s[i];
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF)
            {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (static_cast<uint32_t>(s[i + 1]) - 0xDC00);
                ++i;
            }
            else if (cp >= 0xD800 && cp <= 0xDFFF)
            {
                cp = 0xFFFD; // a lone surrogate
            }
            if (cp < 0x80)
            {
                out.push_back(static_cast<char>(cp));
            }
            else if (cp < 0x800)
            {
                out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
            else if (cp < 0x10000)
            {
                out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
            else
            {
                out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
        }
        return out;
    }

    // The profile path folded to the one spelling every process derives the same names from: '/' ->
    // '\', ASCII lower-cased (Windows paths compare case-insensitively; the folders Agentmaster makes
    // are ASCII), trailing separators dropped. Pure.
    inline std::wstring FoldProfilePath(std::wstring_view profileDir)
    {
        std::wstring s{ profileDir };
        for (auto& ch : s)
        {
            if (ch == L'/')
            {
                ch = L'\\';
            }
            else if (ch >= L'A' && ch <= L'Z')
            {
                ch = static_cast<wchar_t>(ch - L'A' + L'a');
            }
        }
        while (s.size() > 3 && s.back() == L'\\')
        {
            s.pop_back();
        }
        return s;
    }

    // The first 16 hex digits of SHA-256 of the folded profile path (§4.2): the per-profile part of
    // the pipe and mutex names. Two installs on two profiles get two services; two processes on ONE
    // profile always find the same one. Pure.
    inline std::wstring ProfileHash(std::wstring_view profileDir)
    {
        const std::string hex = Sha256Hex(Utf16ToUtf8ForHash(FoldProfilePath(profileDir)));
        return std::wstring(hex.begin(), hex.begin() + 16);
    }

    // \\.\pipe\agentmaster.sessiondb.<hash> — the service's named pipe. Pure.
    inline std::wstring PipeName(std::wstring_view profileDir)
    {
        return L"\\\\.\\pipe\\agentmaster.sessiondb." + ProfileHash(profileDir);
    }

    // Local\Agentmaster.SessionDb.<hash> — the one-service-per-profile mutex (held for the service's
    // life; a second service that cannot take it exits at once). Pure.
    inline std::wstring MutexName(std::wstring_view profileDir)
    {
        return L"Local\\Agentmaster.SessionDb." + ProfileHash(profileDir);
    }

    // Local\Agentmaster.SessionDb.Start.<hash> — taken by a CLIENT around "is a service running? if
    // not, start one", so two clients that find no service at the same moment start ONE (the loser
    // of the race then finds the winner's pipe). Pure.
    inline std::wstring StartMutexName(std::wstring_view profileDir)
    {
        return L"Local\\Agentmaster.SessionDb.Start." + ProfileHash(profileDir);
    }

    // <profile>\sessiondb — every file of the database lives here (§4.3). Pure.
    inline std::wstring ServiceDir(std::wstring_view profileDir)
    {
        std::wstring d{ profileDir };
        while (!d.empty() && (d.back() == L'\\' || d.back() == L'/'))
        {
            d.pop_back();
        }
        return d + L"\\sessiondb";
    }
    // <profile>\sessiondb\sessions.duckdb (+ .wal beside it).
    inline std::wstring DatabasePath(std::wstring_view profileDir)
    {
        return ServiceDir(profileDir) + L"\\sessions.duckdb";
    }
    // <profile>\sessiondb\tmp — DuckDB's spill folder.
    inline std::wstring TempDir(std::wstring_view profileDir)
    {
        return ServiceDir(profileDir) + L"\\tmp";
    }
    // <profile>\sessiondb\bin — the copied-out service + duckdb.dll (§4.2 "Deploy and update").
    inline std::wstring BinDir(std::wstring_view profileDir)
    {
        return ServiceDir(profileDir) + L"\\bin";
    }
    // <profile>\sessiondb\sessiondb.log — the service's own log ([sessdb] lines).
    inline std::wstring LogPath(std::wstring_view profileDir)
    {
        return ServiceDir(profileDir) + L"\\sessiondb.log";
    }
    // <profile>\sessiondb\service.json — {pipe, pid, build, schema, startedAt}, the bridge.json idiom:
    // a human (or a support script) can see which service owns the profile without a client.
    inline std::wstring DiscoveryPath(std::wstring_view profileDir)
    {
        return ServiceDir(profileDir) + L"\\service.json";
    }

    // The service executable's leaf, and DuckDB's DLL beside it.
    inline constexpr std::wstring_view kServiceExeLeaf = L"agentmaster-sessiondb.exe";
    inline constexpr std::wstring_view kDuckDbDllLeaf = L"duckdb.dll";

    // ===== framing ===============================================================================

    // Encode one frame: the 4-byte little-endian payload length, then the payload (UTF-8 JSON).
    // Pure; never throws beyond std::bad_alloc.
    inline std::string EncodeFrame(std::string_view payload)
    {
        std::string out;
        out.resize(4 + payload.size());
        const uint32_t n = static_cast<uint32_t>(payload.size());
        out[0] = static_cast<char>(n & 0xFF);
        out[1] = static_cast<char>((n >> 8) & 0xFF);
        out[2] = static_cast<char>((n >> 16) & 0xFF);
        out[3] = static_cast<char>((n >> 24) & 0xFF);
        if (!payload.empty())
        {
            out.replace(4, payload.size(), payload);
        }
        return out;
    }

    // Decode a frame header's length from its 4 bytes (little-endian). Pure.
    inline uint32_t DecodeFrameLength(const unsigned char header[4]) noexcept
    {
        return static_cast<uint32_t>(header[0]) | (static_cast<uint32_t>(header[1]) << 8) |
               (static_cast<uint32_t>(header[2]) << 16) | (static_cast<uint32_t>(header[3]) << 24);
    }
}
