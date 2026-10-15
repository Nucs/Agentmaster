// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — `agentmaster-sessiondb.exe mcp`: the session database as a stdio MCP server for Claude
// Code (SESSIONS_DB.md §8.4), server name `agentmaster`, so its tools read mcp__agentmaster__search and so
// on. Registered by the user (D6): claude mcp add --scope user agentmaster -- "<exe>" mcp
//
// It is a pipe CLIENT of the profile's service (DuckDB's lock lets one process hold the file): tools/list
// answers from the registry compiled into this binary; tools/call forwards to the service's `toolCall`,
// starting the service when none runs. Every result is the tool's compact JSON as text.
//
// Transport: JSON-RPC 2.0, one message per line on stdin / stdout (UTF-8). Nothing but protocol messages
// ever goes to stdout (logs go to the service log). Each tools/call runs on its own thread with its own pipe
// connection, so a slow tool never blocks a ping or a cancel; notifications/cancelled interrupts the call's
// query in the service.

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "DuckDbWrap.h" // ToUtf8 / FromUtf8
#include "SessionDbLog.h"
#include "SessionDbTools.h"

#include "../ClaudeSpawn.h" // LogSwallowedException
#include "../SessionDbClient.h"
#include "../SessionDbProtocol.h"

namespace Agentmaster::SessionDb
{
    namespace
    {
        using json = nlohmann::json;
        namespace Proto = SessionDbProtocol;

        // The MCP protocol versions this server speaks (the client's is echoed when it is one of them).
        const char* const kProtocolVersions[] = { "2025-06-18", "2025-03-26", "2024-11-05" };
        // How long one tools/call may take end to end (the service's own budgets are far shorter).
        constexpr uint32_t kCallTimeoutMs = 30000;
        // How long a first call waits for a service it had to start (a cold database opens quickly; the
        // sweep runs in the background).
        constexpr uint32_t kStartWaitMs = 15000;

        const char* kInstructions =
            "Search and read the Claude Code sessions on this computer through Agentmaster's session database. Start with `search` (words, scopes, "
            "a time window); open a hit with `messages` and its `ref`; check a session with `session`; find exact text with `grep`; see what a "
            "session touched with `files` or who touched a file with `who_touched`; `timeline`, `usage` and `related` for overviews; `sql` "
            "(read-only views) when nothing else fits. Every tool is read-only.";

        class McpServer
        {
        public:
            explicit McpServer(std::wstring profile) :
                _profile(std::move(profile))
            {
            }

            int Run();

        private:
            void _send(const json& msg);
            void _handle(const json& msg);
            void _toolCall(json id, json params);

            std::wstring _profile;
            std::mutex _outMtx;
            std::mutex _callsMtx;
            std::map<std::string, std::shared_ptr<ServiceClient>> _calls; // in-flight tools/call by JSON-RPC id
            std::atomic<int> _running{ 0 };
        };

        std::string IdKey(const json& id)
        {
            return id.dump();
        }

        void McpServer::_send(const json& msg)
        {
            std::string line = msg.dump(-1, ' ', false, json::error_handler_t::replace);
            line.push_back('\n');
            std::lock_guard lk{ _outMtx };
            std::fwrite(line.data(), 1, line.size(), stdout);
            std::fflush(stdout);
        }

        void McpServer::_toolCall(json id, json params)
        {
            const std::string name = params.value("name", std::string{});
            const json args = params.contains("arguments") ? params["arguments"] : json::object();
            auto client = std::make_shared<ServiceClient>(_profile);
            {
                std::lock_guard lk{ _callsMtx };
                _calls[IdKey(id)] = client;
            }
            json result;
            std::wstring error;
            std::string answer;
            if (!client->Connect(true, kStartWaitMs, error) ||
                !client->Call(Proto::kToolCall, json{ { "name", name }, { "arguments", args } }.dump(-1, ' ', false, json::error_handler_t::replace), answer,
                              kCallTimeoutMs, error))
            {
                result = { { "content", json::array({ { { "type", "text" }, { "text", "The session database service is not available: " + ToUtf8(error) } } }) },
                           { "isError", true } };
            }
            else
            {
                const json env = json::parse(answer, nullptr, false);
                if (env.is_object() && env.contains("result") && env["result"].is_object())
                {
                    const json& r = env["result"];
                    result = { { "content", json::array({ { { "type", "text" }, { "text", r.value("text", std::string{}) } } }) }, { "isError", r.value("isError", false) } };
                }
                else
                {
                    const std::string message = env.is_object() && env.contains("error") ? env["error"].value("message", std::string{ "failed" }) : std::string{ "failed" };
                    result = { { "content", json::array({ { { "type", "text" }, { "text", "The " + name + " call failed: " + message } } }) }, { "isError", true } };
                }
            }
            {
                std::lock_guard lk{ _callsMtx };
                _calls.erase(IdKey(id));
            }
            _send({ { "jsonrpc", "2.0" }, { "id", id }, { "result", result } });
        }

        void McpServer::_handle(const json& msg)
        {
            if (!msg.is_object() || !msg.contains("method") || !msg["method"].is_string())
            {
                if (msg.is_object() && msg.contains("id"))
                {
                    _send({ { "jsonrpc", "2.0" }, { "id", msg["id"] }, { "error", { { "code", -32600 }, { "message", "Invalid request" } } } });
                }
                return; // a response or a malformed notification: nothing to answer
            }
            const std::string method = msg["method"].get<std::string>();
            const bool hasId = msg.contains("id");
            const json id = hasId ? msg["id"] : json(nullptr);
            const json params = msg.contains("params") && msg["params"].is_object() ? msg["params"] : json::object();

            if (method == "initialize")
            {
                std::string version = kProtocolVersions[0];
                const std::string asked = params.value("protocolVersion", std::string{});
                for (const char* v : kProtocolVersions)
                {
                    if (asked == v)
                    {
                        version = v;
                    }
                }
                _send({ { "jsonrpc", "2.0" },
                        { "id", id },
                        { "result",
                          { { "protocolVersion", version },
                            { "capabilities", { { "tools", { { "listChanged", false } } } } },
                            { "serverInfo", { { "name", "agentmaster" }, { "title", "Agentmaster sessions" }, { "version", std::to_string(Proto::kToolRegistryVersion) } } },
                            { "instructions", kInstructions } } } });
                return;
            }
            if (method == "notifications/initialized" || method == "initialized")
            {
                return;
            }
            if (method == "ping")
            {
                if (hasId)
                {
                    _send({ { "jsonrpc", "2.0" }, { "id", id }, { "result", json::object() } });
                }
                return;
            }
            if (method == "tools/list")
            {
                _send({ { "jsonrpc", "2.0" }, { "id", id }, { "result", { { "tools", ToolDefinitionsJson() } } } });
                return;
            }
            if (method == "tools/call")
            {
                ++_running;
                try
                {
                    std::thread([this, id, params] {
                        try
                        {
                            _toolCall(id, params);
                        }
                        catch (...)
                        {
                            LogSwallowedException(L"sessiondb mcp tools/call");
                            _send({ { "jsonrpc", "2.0" }, { "id", id }, { "error", { { "code", -32603 }, { "message", "Internal error" } } } });
                        }
                        --_running;
                    }).detach();
                }
                catch (...)
                {
                    LogSwallowedException(L"sessiondb mcp thread");
                    --_running;
                    _send({ { "jsonrpc", "2.0" }, { "id", id }, { "error", { { "code", -32603 }, { "message", "Internal error" } } } });
                }
                return;
            }
            if (method == "notifications/cancelled")
            {
                const std::string key = params.contains("requestId") ? IdKey(params["requestId"]) : std::string{};
                std::shared_ptr<ServiceClient> client;
                {
                    std::lock_guard lk{ _callsMtx };
                    const auto it = _calls.find(key);
                    if (it != _calls.end())
                    {
                        client = it->second;
                    }
                }
                if (client)
                {
                    client->CancelPending();
                }
                return;
            }
            if (hasId)
            {
                _send({ { "jsonrpc", "2.0" }, { "id", id }, { "error", { { "code", -32601 }, { "message", "Method not found: " + method } } } });
            }
        }

        int McpServer::Run()
        {
            // Binary stdout: the C runtime's text mode would turn every '\n' into "\r\n" inside the framing.
            _setmode(_fileno(stdout), _O_BINARY);
            ServiceLog(L"[sessdb] mcp server started (pid " + std::to_wstring(::GetCurrentProcessId()) + L")");
            const HANDLE in = ::GetStdHandle(STD_INPUT_HANDLE);
            std::string buffer;
            char chunk[64 * 1024];
            for (;;)
            {
                DWORD n = 0;
                if (!::ReadFile(in, chunk, sizeof(chunk), &n, nullptr) || n == 0)
                {
                    break; // the client closed stdin: the session ended
                }
                buffer.append(chunk, n);
                size_t start = 0;
                for (size_t nl = buffer.find('\n', start); nl != std::string::npos; nl = buffer.find('\n', start))
                {
                    std::string line = buffer.substr(start, nl - start);
                    start = nl + 1;
                    if (!line.empty() && line.back() == '\r')
                    {
                        line.pop_back();
                    }
                    if (line.empty())
                    {
                        continue;
                    }
                    const json msg = json::parse(line, nullptr, false);
                    if (msg.is_discarded())
                    {
                        _send({ { "jsonrpc", "2.0" }, { "id", nullptr }, { "error", { { "code", -32700 }, { "message", "Parse error" } } } });
                        continue;
                    }
                    try
                    {
                        if (msg.is_array())
                        {
                            for (const json& m : msg)
                            {
                                _handle(m); // a JSON-RPC batch (older protocol versions)
                            }
                        }
                        else
                        {
                            _handle(msg);
                        }
                    }
                    catch (...)
                    {
                        LogSwallowedException(L"sessiondb mcp message");
                    }
                }
                buffer.erase(0, start);
            }
            // Let in-flight calls finish their answer (they hold this object); 5 s at most.
            for (int i = 0; i < 100 && _running.load() > 0; ++i)
            {
                ::Sleep(50);
            }
            ServiceLog(L"[sessdb] mcp server stopped");
            return 0;
        }
    }

    int RunMcp(const std::wstring& profileDir)
    {
        McpServer s{ profileDir };
        return s.Run();
    }
}
