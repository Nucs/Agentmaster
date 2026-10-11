// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Plain C++ engine TU — no WinRT, no precompiled header (vcxproj marks it NotUsing).
// The conversation object of the agent harness: the ONE file of Agentmaster that talks to the
// Claude Agent SDK's client. See AgentHarness.h and SDK_HARNESS.md.
#include "AgentHarness.h"

#include "ClaudeSpawn.h" // AppendStateLog, LogSwallowedException

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

// The SDK's headers are written for other warning settings than the app's /W3 /WX: transport.hpp
// declares `class ClaudeOptions;` for a struct (C4099). Nothing of Agentmaster is inside this
// region. (ClaudeSdkUnity.cpp compiles the SDK's own sources, with its own list.)
#pragma warning(push)
#pragma warning(disable : 4099 4100)
#include <claude/client.hpp>
#include <claude/errors.hpp>
#include <claude/mcp/server.hpp>
#include <claude/transport.hpp>
#include <claude/types.hpp>

#include "internal/transport/subprocess_transport.hpp" // SubprocessTransport::build_command — DescribeCommandLine only
#pragma warning(pop)

namespace
{
    using namespace Agentmaster;

    using SdkJson = claude::json; // nlohmann::json

    int64_t NowUnixMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // One [harness] line in hooks.log: "<name>: <message>". Never throws.
    void LogHarness(const std::wstring& name, const std::wstring& message) noexcept
    {
        try
        {
            AppendStateLog(L"hooks.log", L"[harness] " + name + L": " + message + L"\n");
        }
        catch (...)
        {
            // ⚠ DELIBERATELY BARE (CLAUDE.md Rule #18, the logging-path exemption): this IS the
            // logging path; reporting its own failure would call AppendStateLog again.
        }
    }

    // nlohmann JSON -> text. `replace` instead of the default `strict`: a string that is not
    // valid UTF-8 must give text with U+FFFD, not an exception on the SDK's reader thread.
    std::wstring SdkJsonText(const SdkJson& j)
    {
        return HarnessFromUtf8(j.dump(-1, ' ', false, SdkJson::error_handler_t::replace));
    }

    // True when `name` can be a tool or server name: letters, digits, '_' and '-'. The full
    // tool name, mcp__<server>__<tool>, goes on the command line (--allowedTools) and to the API.
    bool IsValidToolName(std::wstring_view name) noexcept
    {
        if (name.empty() || name.size() > 64)
        {
            return false;
        }
        for (const wchar_t c : name)
        {
            const bool ok = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'_' || c == L'-';
            if (!ok)
            {
                return false;
            }
        }
        return true;
    }

    // Write `text` as UTF-8 (no BOM) to `path`, replacing the file.
    //   returns — true when every byte was written.
    //   throws  — std::bad_alloc.
    bool WriteTextFileUtf8(const std::wstring& path, std::wstring_view text)
    {
        const std::string bytes = HarnessToUtf8(text);
        const HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        DWORD wrote = 0;
        const BOOL ok = bytes.empty() ? TRUE : ::WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr);
        ::CloseHandle(h);
        return ok && wrote == bytes.size();
    }

    // The shape the SDK's MCP server builder (claude::mcp::ToolStorage) expects of a tool: a type
    // with name / description / input_schema / output_schema / invoke. The SDK's own ToolWrapper
    // derives the schema from a C++ function signature (parameters named arg0, arg1, ...); a
    // harness tool brings a written schema and takes the raw arguments instead.
    class DynamicTool
    {
    public:
        // ⚠ The two JSON members are initialized with PARENTHESES on purpose. nlohmann::json has an
        // initializer-list constructor, so `_inputSchema{ json }` builds a one-element ARRAY that
        // holds the schema — Claude Code then rejects the whole tool list (it asks for it again
        // and again, and the model ends up without the tools; found in the first live run).
        DynamicTool(std::string name, std::string description, SdkJson inputSchema, std::function<SdkJson(const SdkJson&)> invoker) :
            _name{ std::move(name) },
            _description{ std::move(description) },
            _inputSchema(std::move(inputSchema)),
            _outputSchema(SdkJson::object()),
            _invoker{ std::move(invoker) }
        {
        }

        const std::string& name() const { return _name; }
        const std::string& description() const { return _description; }
        const SdkJson& input_schema() const { return _inputSchema; }
        const SdkJson& output_schema() const { return _outputSchema; }

        // The MCP `tools/call` result object for `args`.
        SdkJson invoke(const SdkJson& args) const { return _invoker(args); }

    private:
        std::string _name;
        std::string _description;
        SdkJson _inputSchema;
        SdkJson _outputSchema; // the SDK asks for one; Claude Code never reads it
        std::function<SdkJson(const SdkJson&)> _invoker;
    };

    // Wraps the SDK's subprocess transport to report ONE thing the SDK misses: the child's exit.
    //
    // The SDK client's reader loop stops quietly when the transport is no longer running, and
    // leaves every control request that waits for an answer waiting — `initialize` for 60 s when
    // Claude Code ends at once (an option it does not know, a broken install), `interrupt` the
    // same. This wrapper keeps the loop alive until the transport's queue is drained, then THROWS
    // from read_messages: the client's loop catches that, stops its message queue and fails the
    // pending requests at once. Everything else is forwarded unchanged.
    class WatchedTransport final : public claude::Transport
    {
    public:
        explicit WatchedTransport(std::unique_ptr<claude::Transport> inner) :
            _inner{ std::move(inner) }
        {
        }

        void connect() override { _inner->connect(); }
        void write(const std::string& data) override { _inner->write(data); }

        std::vector<claude::Message> read_messages() override
        {
            auto messages = _inner->read_messages();
            if (!messages.empty() || _inner->is_running())
            {
                _emptyReadsAfterExit = 0;
                return messages;
            }
            // The child is gone and this read brought nothing. The transport's own reader thread
            // may still be handing over the last lines, so give it a few more reads (each waits
            // up to 100 ms inside the SDK) before the exit is reported.
            if (++_emptyReadsAfterExit < kDrainReads)
            {
                return messages;
            }
            _exitReported.store(true);
            throw claude::CLIConnectionError("The Claude Code process ended.");
        }

        // True until the exit was reported, so the client's loop keeps calling read_messages
        // (it stops by itself when both this and is_running say no).
        bool has_messages() const override { return _inner->has_messages() || !_exitReported.load(); }
        void close() override { _inner->close(); }
        bool is_ready() const override { return _inner->is_ready(); }
        void end_input() override { _inner->end_input(); }
        long get_pid() const override { return _inner->get_pid(); }
        bool is_running() const override { return _inner->is_running() || !_exitReported.load(); }

    private:
        static constexpr int kDrainReads = 5;
        std::unique_ptr<claude::Transport> _inner;
        int _emptyReadsAfterExit{ 0 }; // the client's reader thread only
        std::atomic<bool> _exitReported{ false };
    };
}

namespace Agentmaster
{
    const wchar_t* HarnessStateName(HarnessState state) noexcept
    {
        switch (state)
        {
        case HarnessState::Stopped:
            return L"stopped";
        case HarnessState::Starting:
            return L"starting";
        case HarnessState::Ready:
            return L"ready";
        case HarnessState::Running:
            return L"running";
        case HarnessState::Failed:
        default:
            return L"failed";
        }
    }

    struct HarnessConversation::Impl
    {
        // ---- fixed after construction ----
        HarnessOptions options;
        EventSink sink;
        std::wstring workDir; // the child's working folder
        std::wstring recordPath; // empty when the record is not persisted
        std::wstring promptPath; // <workDir>\system-prompt.md

        // ---- shared state; `mutex` guards all of it ----
        mutable std::mutex mutex;
        std::condition_variable wake; // signaled when the worker has something to do
        HarnessState state{ HarnessState::Stopped };
        uint64_t epoch{ 1 };
        uint64_t nextTurn{ 1 };
        struct Pending
        {
            uint64_t turn{};
            std::wstring text;
        };
        std::optional<Pending> pending; // the newest message that did not start yet
        bool wantProcess{ false }; // Prewarm / Reset asked for a process
        bool resetRequested{ false };
        bool stopRequested{ false };
        // ParkAfter: when the idle process is to be ended. Empty = no park is pending. Cleared by
        // Prewarm / Send / Reset (somebody needs the process) and by the worker when it parks.
        std::optional<std::chrono::steady_clock::time_point> parkAt;
        HarnessRecord record;
        // What the newest process reported about itself (Inventory()). Written by the worker:
        // the permission fields when a process answered `initialize`, the lists at its first turn.
        HarnessInventory inventory;

        // ---- cross-thread flags ----
        // The permission mode the NEXT start asks for. Bypass, until a start with it was refused
        // with an error AND a start with a lower mode then worked: from then on the lower mode,
        // for this object's life (what refuses a mode is a policy, and a policy does not change
        // between two starts; a new app run asks for bypass again). Written by the worker only;
        // atomic because DescribeCommandLine reads it from other threads.
        std::atomic<HarnessPermissionMode> launchPermission{ HarnessPermissionMode::Bypass };
        std::atomic<bool> interruptRequested{ false };
        std::atomic<int> toolCallsInFlight{ 0 }; // > 0: a tool handler runs on the SDK reader thread
        std::atomic<uint64_t> activeTurn{ 0 }; // the turn tool events belong to
        std::atomic<uint64_t> activeTurnEpoch{ 0 }; // the epoch that turn started in

        // One sink call at a time. Recursive: the sink may call Send / Reset, which report the
        // message they replaced on the calling thread — that is, from inside the sink.
        std::recursive_mutex sinkMutex;
        std::mutex stderrMutex; // guards stderrTail + stderrLogged
        std::deque<std::wstring> stderrTail; // the child's last stderr lines, for failure messages
        int stderrLogged{ 0 };

        // ---- worker thread only ----
        std::unique_ptr<claude::ClaudeClient> client;
        bool processIsFresh{ false }; // the process has not received a user message yet
        bool readyAnnounced{ false }; // the Ready event of this process was sent
        // The mode word of the newest report the permission policy acted on, for this process.
        // A turn's system/init that repeats it is not acted on again: a Claude Code that keeps
        // reporting a mode the harness could not change must not cost two control requests and
        // three log lines at every turn.
        std::wstring lastSettledReport;
        double costAtProcessStart{ 0 }; // record.usage.costUsd when this process started (its own cost counts from 0)

        std::thread worker; // LAST: started when everything above exists

        // -------------------------------------------------------------------------------
        // events
        // -------------------------------------------------------------------------------

        // Send `e` to the sink. Fills e.epoch (unless the caller set one), e.state and e.usage
        // from the current state. Never throws: a sink that throws is logged and ignored.
        void Emit(HarnessEvent e) noexcept
        {
            try
            {
                {
                    std::lock_guard lock{ mutex };
                    if (e.epoch == 0)
                    {
                        e.epoch = epoch;
                    }
                    e.state = state;
                    e.usage = record.usage;
                }
                if (!sink)
                {
                    return;
                }
                std::lock_guard<std::recursive_mutex> sinkLock{ sinkMutex };
                sink(e);
            }
            catch (...)
            {
                LogSwallowedException(L"HarnessConversation event sink");
            }
        }

        // Change the state and report it.
        //   kind — State for a plain change; Error / Notice when `text` explains it.
        void SetState(HarnessState s, HarnessEventKind kind = HarnessEventKind::State, std::wstring text = {}, bool isError = false) noexcept
        {
            {
                std::lock_guard lock{ mutex };
                state = s;
            }
            HarnessEvent e;
            e.kind = kind;
            e.text = std::move(text);
            e.isError = isError;
            Emit(std::move(e));
        }

        // True when `turnEpoch` is still the conversation's epoch — when the conversation was not
        // reset since the turn started. A stale turn must not write into the new, empty record.
        bool EpochIsCurrent(uint64_t turnEpoch) const
        {
            std::lock_guard lock{ mutex };
            return epoch == turnEpoch;
        }

        // Append an item to the record, unless the turn that produced it is stale.
        void Record(uint64_t turnEpoch, HarnessItem item) noexcept
        {
            try
            {
                std::lock_guard lock{ mutex };
                if (epoch != turnEpoch)
                {
                    return;
                }
                if (record.createdUnixMs == 0)
                {
                    record.createdUnixMs = item.atUnixMs;
                }
                AppendHarnessItem(record, std::move(item));
            }
            catch (...)
            {
                LogSwallowedException(L"HarnessConversation::Record");
            }
        }

        // Write the record to its file (when it has one). Called after every turn and reset.
        void Persist() noexcept
        {
            if (recordPath.empty())
            {
                return;
            }
            try
            {
                HarnessRecord copy;
                {
                    std::lock_guard lock{ mutex };
                    copy = record;
                }
                SaveHarnessRecordTo(recordPath, copy); // logs its own failure
            }
            catch (...)
            {
                LogSwallowedException(L"HarnessConversation::Persist");
            }
        }

        // The child's last stderr lines as one text, for a failure message ("" when none).
        std::wstring StderrTailText()
        {
            std::lock_guard lock{ stderrMutex };
            std::wstring out;
            for (const auto& line : stderrTail)
            {
                if (!out.empty())
                {
                    out += L" | ";
                }
                out += line;
            }
            return out;
        }

        // -------------------------------------------------------------------------------
        // the SDK options
        // -------------------------------------------------------------------------------

        // One tool call, on the SDK reader thread: report it, run the handler, report the answer,
        // and build the MCP `tools/call` result.
        SdkJson InvokeTool(const std::wstring& fullName, const HarnessTool& tool, const SdkJson& args) noexcept
        {
            // Counted so the worker's silence watchdog does not take a slow tool for a hung turn.
            toolCallsInFlight.fetch_add(1);
            HarnessToolResult result;
            try
            {
                const uint64_t turn = activeTurn.load();
                const uint64_t turnEpoch = activeTurnEpoch.load();
                const std::wstring argsText = SdkJsonText(args);

                HarnessItem call;
                call.kind = HarnessItemKind::ToolCall;
                call.turn = turn;
                call.atUnixMs = NowUnixMs();
                call.toolName = fullName;
                call.argumentsJson = argsText;
                Record(turnEpoch, call);
                if (EpochIsCurrent(turnEpoch))
                {
                    HarnessEvent e;
                    e.kind = HarnessEventKind::ToolCall;
                    e.epoch = turnEpoch;
                    e.turn = turn;
                    e.toolName = fullName;
                    e.argumentsJson = argsText;
                    Emit(std::move(e));
                }

                try
                {
                    auto parsed = json::Parse(argsText);
                    // The model sends an object. Anything else (it cannot be more than a model
                    // error) reaches the handler as an empty object, which its own checks reject.
                    const json::Value arguments = (parsed && parsed->type == json::Value::Type::Obj) ? std::move(*parsed) : json::Value::MkObj();
                    result = tool.handler ? tool.handler(arguments) : HarnessToolResult{ L"This tool has no handler.", true };
                }
                catch (const std::exception& ex)
                {
                    LogSwallowedException(L"harness tool handler");
                    result = HarnessToolResult{ L"The tool failed: " + HarnessFromUtf8(ex.what()), true };
                }
                catch (...)
                {
                    LogSwallowedException(L"harness tool handler");
                    result = HarnessToolResult{ L"The tool failed.", true };
                }

                HarnessItem answer;
                answer.kind = HarnessItemKind::ToolResult;
                answer.turn = turn;
                answer.atUnixMs = NowUnixMs();
                answer.toolName = fullName;
                answer.text = result.text; // the record keeps a preview only (AppendHarnessItem cuts it)
                answer.isError = result.isError;
                Record(turnEpoch, answer);
                if (EpochIsCurrent(turnEpoch))
                {
                    HarnessEvent e;
                    e.kind = HarnessEventKind::ToolResult;
                    e.epoch = turnEpoch;
                    e.turn = turn;
                    e.toolName = fullName;
                    e.text = result.text;
                    e.isError = result.isError;
                    Emit(std::move(e));
                }
            }
            catch (...)
            {
                // Out of memory while reporting. The model still gets an answer.
                LogSwallowedException(L"HarnessConversation::InvokeTool");
                if (result.text.empty())
                {
                    result.isError = true;
                }
            }
            toolCallsInFlight.fetch_sub(1);
            try
            {
                return SdkJson{
                    { "content", SdkJson::array({ SdkJson{ { "type", "text" }, { "text", HarnessToUtf8(result.text) } } }) },
                    { "isError", result.isError },
                };
            }
            catch (...)
            {
                LogSwallowedException(L"HarnessConversation::InvokeTool (result)");
                return SdkJson{ { "content", SdkJson::array() }, { "isError", true } };
            }
        }

        // The SDK options of this conversation.
        //   cliPath    — the narrow claude.exe path (NarrowPathForSdk); only stored, never opened.
        //   permission — the permission mode this start asks for (launchPermission, or the next
        //                lower mode after a refused start).
        //   throws     — std::invalid_argument when a tool's inputSchemaJson is not a JSON object;
        //                std::bad_alloc.
        claude::ClaudeOptions BuildSdkOptions(const std::string& cliPath, HarnessPermissionMode permission)
        {
            claude::ClaudeOptions o;
            o.cli_path = cliPath;
            o.require_explicit_cli = true; // never search PATH: there the shim and npm launchers live
            o.model = HarnessToUtf8(options.model);
            o.working_directory = HarnessToUtf8(workDir);

            claude::SystemPromptFile prompt;
            prompt.path = HarnessToUtf8(promptPath);
            o.system_prompt_file = prompt;

            // The model can call ONLY our tools: no built-in tool, no MCP server of the user.
            // ⚠ THIS is the limit of what the model can do. The permission mode below limits
            // nothing in bypass.
            o.tools = std::vector<std::string>{}; // --tools ""
            o.strict_mcp_config = true;
            // The permission policy (AgentHarnessCore.h): bypass always; auto, then dontAsk, only
            // after Claude Code refused. Whether it was GRANTED is read from the process
            // (SettlePermissionMode) — Claude Code lowers a mode it does not allow without a word.
            o.permission_mode = HarnessToUtf8(HarnessPermissionModeWord(permission));
            if (permission == HarnessPermissionMode::Bypass)
            {
                // The same mode by its other name. Both are passed: each alone selects bypass
                // (measured), and a Claude Code that drops one of them still gets the other.
                o.extra_args["--dangerously-skip-permissions"] = "";
            }
            if (options.permissionProbe.restricted)
            {
                o.extra_args["--restricted"] = ""; // test seam: bypass is then refused with an error at start
            }
            o.setting_sources.clear(); // --setting-sources "": no user / project / local settings, no CLAUDE.md
            HarnessSettingsSpec settings;
            settings.autoCompactTokens = options.autoCompactTokens;
            settings.effort = options.effort;
            settings.apiMaxRetries = options.apiMaxRetries;
            settings.disableBypassPermissions = options.permissionProbe.bypassDisabledBySettings; // test seam
            settings.disableAutoMode = options.permissionProbe.autoDisabledBySettings; // test seam
            o.settings = HarnessToUtf8(BuildHarnessSettingsJson(settings));

            // The whole parent environment, minus the variables of a parent session or tab.
            o.sanitize_environment = false;
            o.inherit_environment = true;
            o.environment = BuildHarnessEnvironment(options.name);

            switch (options.thinking)
            {
            case HarnessThinking::Disabled:
                o.thinking = claude::ThinkingConfig{ claude::ThinkingConfigDisabled{} }; // --thinking disabled
                break;
            case HarnessThinking::Adaptive:
                o.thinking = claude::ThinkingConfig{ claude::ThinkingConfigAdaptive{} }; // --thinking adaptive
                break;
            case HarnessThinking::CliDefault:
            default:
                break;
            }
            if (!options.effort.empty())
            {
                o.effort = HarnessToUtf8(options.effort);
            }
            if (options.maxTurnsPerMessage > 0)
            {
                o.max_turns = options.maxTurnsPerMessage;
            }
            o.include_partial_messages = options.partialMessages;

            // Flags the SDK has no option for. (A map: the SDK appends them sorted by name.)
            if (!options.claudePersistsSession)
            {
                o.extra_args["--no-session-persistence"] = "";
            }
            if (const int64_t window = ClampHarnessAutoCompactTokens(options.autoCompactTokens); window != 0)
            {
                o.extra_args["--autocompact"] = std::to_string(window);
            }
            o.extra_args["--disable-slash-commands"] = ""; // a message that starts with '/' is text, not a command

            for (const auto& server : options.toolServers)
            {
                const std::string serverName = HarnessToUtf8(server.name);
                claude::mcp::ServerBuilder builder{ serverName, HarnessToUtf8(server.version) };
                for (const auto& tool : server.tools)
                {
                    SdkJson schema;
                    try
                    {
                        schema = SdkJson::parse(HarnessToUtf8(tool.inputSchemaJson));
                    }
                    catch (const std::exception&)
                    {
                        // Expected control flow for a wrong schema text: rethrown as the documented
                        // std::invalid_argument, whose message the caller reports.
                        throw std::invalid_argument("The input schema of tool '" + HarnessToUtf8(tool.name) + "' is not valid JSON.");
                    }
                    if (!schema.is_object())
                    {
                        throw std::invalid_argument("The input schema of tool '" + HarnessToUtf8(tool.name) + "' is not a JSON object.");
                    }
                    const std::wstring fullName = HarnessToolFullName(server.name, tool.name);
                    claude::mcp::ToolAnnotations annotations;
                    annotations.read_only_hint = tool.readOnly;
                    // `tool` is copied into the callable: the SDK keeps it for the client's life,
                    // and `this` outlives the client (Disconnect destroys the client first).
                    builder.add_tool(
                        DynamicTool{ HarnessToUtf8(tool.name), HarnessToUtf8(tool.description), std::move(schema), [this, fullName, tool](const SdkJson& args) { return InvokeTool(fullName, tool, args); } },
                        annotations);
                }
                auto handler = builder.build();
                // The SDK's handler answers a notification (a message without an id, e.g.
                // notifications/initialized) with "method not found". The Python SDK answers an
                // empty result; do the same, so Claude Code never sees an error for it.
                o.sdk_mcp_handlers[serverName] = [this, handler, wideServer = server.name](const SdkJson& request) -> SdkJson {
                    const std::string method = request.is_object() ? request.value("method", std::string{}) : std::string{};
                    if (method.rfind("notifications/", 0) == 0)
                    {
                        return SdkJson{ { "jsonrpc", "2.0" }, { "result", SdkJson::object() } };
                    }
                    SdkJson response = handler(request);
                    if (method == "initialize")
                    {
                        // The SDK's server says that it has RESOURCES. It has none (it answers
                        // resources/list with "method not found"), and Claude Code answers the
                        // claim by giving the model three more tools: ListMcpResourcesTool,
                        // ReadMcpResourceTool and ReadMcpResourceDirTool. They add 769 tokens to
                        // every request (measured, Claude Code 2.1.291: 2,224 instead of 1,455
                        // for two tools), they can only fail, and with bypass permissions nothing
                        // refuses them. A harness server offers tools and nothing else.
                        if (const auto result = response.find("result"); result != response.end() && result->is_object())
                        {
                            if (const auto capabilities = result->find("capabilities"); capabilities != result->end() && capabilities->is_object())
                            {
                                capabilities->erase("resources");
                            }
                        }
                    }
                    if (method == "tools/list")
                    {
                        // Once per process start: what Claude Code was told this server offers. A
                        // model that "has no such tool" is diagnosed from this line.
                        size_t count = 0;
                        if (const auto result = response.find("result"); result != response.end() && result->is_object())
                        {
                            if (const auto tools = result->find("tools"); tools != result->end() && tools->is_array())
                            {
                                count = tools->size();
                            }
                        }
                        LogHarness(options.name, L"tool server '" + wideServer + L"' listed " + std::to_wstring(count) + L" tool(s)");
                    }
                    return response;
                };
                // The whole server, on the allow list. Bypass does not read the list; the two
                // fallback modes do: in auto an allowed tool runs at once (no classifier request),
                // in dontAsk it is the only kind of tool that runs at all.
                o.allowed_tools.push_back("mcp__" + serverName);
            }

            o.stderr_callback = [this](const std::string& line) {
                try
                {
                    std::wstring text = HarnessFromUtf8(line);
                    if (text.size() > 500)
                    {
                        text.resize(500);
                    }
                    bool log = false;
                    {
                        std::lock_guard lock{ stderrMutex };
                        stderrTail.push_back(text);
                        while (stderrTail.size() > 8)
                        {
                            stderrTail.pop_front();
                        }
                        log = stderrLogged++ < 30; // a chatty child must not flood hooks.log
                    }
                    if (log)
                    {
                        LogHarness(options.name, L"stderr: " + text);
                    }
                }
                catch (...)
                {
                    LogSwallowedException(L"HarnessConversation stderr callback");
                }
            };
            return o;
        }

        // -------------------------------------------------------------------------------
        // the process
        // -------------------------------------------------------------------------------

        // End the process and destroy the client. Worker thread only.
        //   kill — true: terminate the child first (fast; nothing of it needs to be saved).
        //          false: close its input and let the SDK wait for a clean exit (up to 5 s) —
        //          for a child that writes its own transcript.
        void Disconnect(bool kill) noexcept
        {
            if (!client)
            {
                return;
            }
            try
            {
                if (kill)
                {
                    // The SDK holds the child's process handle, so the id cannot name another
                    // process yet.
                    if (const long pid = client->get_pid(); pid > 0)
                    {
                        if (const HANDLE h = ::OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid)))
                        {
                            ::TerminateProcess(h, 1);
                            ::CloseHandle(h);
                        }
                    }
                }
                // force_disconnect, not disconnect: with in-process tools, disconnect() waits up
                // to 60 s for a turn's result before it closes the input.
                client->force_disconnect();
            }
            catch (...)
            {
                LogSwallowedException(L"HarnessConversation::Disconnect");
            }
            try
            {
                client.reset();
            }
            catch (...)
            {
                LogSwallowedException(L"HarnessConversation::Disconnect (client)");
            }
            processIsFresh = false;
            readyAnnounced = false;
        }

        // ONE start attempt: start claude.exe with `permission` and wait for its `initialize`
        // answer. Worker thread only; no client exists when it is called.
        //   permission — the permission mode this attempt asks for.
        //   returns    — empty when `client` is connected; else why not (the SDK's text plus the
        //                child's last stderr lines). After a failure no client and no process is left.
        // Every failure of the start becomes the returned text. (Out of memory while that text is
        // built ends the process, as for every noexcept function of this file.)
        std::wstring StartOnce(HarnessPermissionMode permission) noexcept
        {
            std::wstring failure;
            try
            {
                const std::wstring exe = options.claudeExe ? options.claudeExe() : std::wstring{};
                if (exe.empty())
                {
                    failure = L"Claude Code (claude.exe) was not found.";
                }
                else if (const auto cliPath = NarrowPathForSdk(exe); !cliPath)
                {
                    failure = L"The path of claude.exe has characters that the system code page cannot hold: " + exe;
                }
                else
                {
                    std::error_code ec;
                    std::filesystem::create_directories(std::filesystem::path{ workDir }, ec);
                    if (!WriteTextFileUtf8(promptPath, options.systemPrompt))
                    {
                        failure = L"The system prompt file could not be written: " + promptPath;
                    }
                    else
                    {
                        {
                            std::lock_guard lock{ stderrMutex };
                            stderrTail.clear();
                            stderrLogged = 0;
                        }
                        const claude::ClaudeOptions sdkOptions = BuildSdkOptions(*cliPath, permission);
                        auto watched = std::make_unique<WatchedTransport>(claude::create_subprocess_transport(sdkOptions));
                        client = std::make_unique<claude::ClaudeClient>(sdkOptions, std::move(watched));
                        client->connect(); // starts claude.exe and waits for its `initialize` answer
                        if (!client->is_connected())
                        {
                            failure = L"Claude Code ended while it started.";
                        }
                    }
                }
            }
            catch (const std::exception& ex)
            {
                LogSwallowedException(L"HarnessConversation::StartOnce");
                failure = L"Claude Code could not be started: " + HarnessFromUtf8(ex.what());
            }
            catch (...)
            {
                LogSwallowedException(L"HarnessConversation::StartOnce");
                failure = L"Claude Code could not be started.";
            }
            if (failure.empty())
            {
                return failure;
            }
            // FIRST end the client: that joins the SDK's stderr reader, so every line the child
            // wrote before it ended is in stderrTail when it is read below. The line is what says
            // WHY Claude Code ended at once — and EnsureProcess decides on it.
            Disconnect(true);
            if (const std::wstring tail = StderrTailText(); !tail.empty())
            {
                failure += L" (" + tail + L")";
            }
            return failure;
        }

        // Ask the running process for another permission mode (the control request
        // set_permission_mode). Worker thread only; `client` is connected.
        //   word       — the mode word ("auto", "dontAsk").
        //   refusalOut — receives Claude Code's reason when it said no.
        //   returns    — true when Claude Code accepted the mode.
        // Never throws. A refusal is an expected answer here ("Cannot set permission mode to
        // auto: auto mode disabled by settings"): the caller goes on to the next mode.
        bool TrySetPermissionMode(const wchar_t* word, std::wstring& refusalOut) noexcept
        {
            try
            {
                client->set_permission_mode(HarnessToUtf8(word)); // waits for the answer; throws on an error answer
                return true;
            }
            catch (const std::exception& ex)
            {
                LogSwallowedException(L"HarnessConversation::TrySetPermissionMode");
                try
                {
                    refusalOut = HarnessFromUtf8(ex.what());
                }
                catch (...)
                {
                    LogSwallowedException(L"HarnessConversation::TrySetPermissionMode (text)");
                }
            }
            catch (...)
            {
                LogSwallowedException(L"HarnessConversation::TrySetPermissionMode");
            }
            return false;
        }

        // Carry out the permission policy for a mode the process REPORTED, and record the mode
        // that is in effect afterwards. Worker thread only; `client` is connected.
        //   asked    — the mode the launch asked for.
        //   reported — the mode Claude Code reported: `current_permission_mode` of its answer to
        //              `initialize`, or `permissionMode` of a turn's system/init line. Empty when
        //              this Claude Code version reports none at start.
        //   where    — "at start" / "at the first turn": where the report came from (log text).
        //   returns  — the mode in effect now (== `reported` when nothing had to change or the
        //              corrections were refused).
        // Never throws. Each outcome is one [harness] line, so hooks.log shows for every process
        // which mode it runs in and why.
        std::wstring SettlePermissionMode(HarnessPermissionMode asked, const std::wstring& reported, const wchar_t* where) noexcept
        {
            std::wstring effective;
            try
            {
                effective = reported;
                lastSettledReport = reported;
                const std::wstring askedWord = HarnessPermissionModeWord(asked);
                std::wstring refusal;
                switch (DecideHarnessPermissionStep(asked, reported))
                {
                case HarnessPermissionStep::Keep:
                    if (reported.empty())
                    {
                        LogHarness(options.name, L"permission mode: asked " + askedWord + L"; this Claude Code reports no mode " + where + L" (it is read from the first turn)");
                    }
                    else if (reported == askedWord)
                    {
                        LogHarness(options.name, L"permission mode " + reported + L" (" + where + L")");
                    }
                    else
                    {
                        // The rule's second half, done by Claude Code itself: a policy forbids
                        // what was asked, and it started in the next mode that never asks.
                        LogHarness(options.name, L"permission mode " + reported + L" (" + where + L"): " + askedWord + L" is NOT ALLOWED here, Claude Code chose " + reported);
                    }
                    break;
                case HarnessPermissionStep::SwitchToAuto:
                    if (TrySetPermissionMode(L"auto", refusal))
                    {
                        effective = L"auto";
                        LogHarness(options.name, L"permission mode auto (" + std::wstring{ where } + L"): " + askedWord + L" is NOT ALLOWED here (Claude Code ran in '" + reported + L"', which can ask a person); switched to auto");
                        break;
                    }
                    LogHarness(options.name, L"permission mode: auto was REFUSED too (" + refusal.substr(0, 300) + L")");
                    [[fallthrough]];
                case HarnessPermissionStep::SwitchToDontAsk:
                    refusal.clear();
                    if (TrySetPermissionMode(L"dontAsk", refusal))
                    {
                        effective = L"dontAsk";
                        LogHarness(options.name, L"permission mode dontAsk (" + std::wstring{ where } + L"): neither bypassPermissions nor auto is allowed here (Claude Code ran in '" + reported + L"'); a tool that is not on the allowed list is refused");
                    }
                    else
                    {
                        // Measured on 2.1.291: a process without a permission handler REFUSES a
                        // tool call that no rule allows in "default" too — it does not wait for
                        // an answer. So what is left here is a mode the harness did not want,
                        // not a process that can hang.
                        LogHarness(options.name, L"permission mode stays '" + reported + L"' (" + where + L"): the switch to dontAsk FAILED (" + refusal.substr(0, 300) + L")");
                    }
                    break;
                }
                std::lock_guard lock{ mutex };
                inventory.known = true;
                inventory.askedPermissionMode = askedWord;
                inventory.permissionMode = effective;
            }
            catch (...)
            {
                LogSwallowedException(L"HarnessConversation::SettlePermissionMode");
            }
            return effective;
        }

        // Make sure a process is up. Worker thread only.
        //   failureOut — receives the reason when the start failed (the text of the Error event).
        //   returns    — true when `client` is connected; false after reporting the failure
        //                (state Failed, an Error event).
        // THE PERMISSION POLICY at start (AgentHarnessCore.h): the start asks for
        // launchPermission (bypass). When Claude Code ENDS with an error that names the
        // permission mode, the start is done again with the next lower mode — at most twice.
        // A mode that Claude Code lowers WITHOUT an error is handled after the start, from the
        // mode the process reports (SettlePermissionMode).
        bool EnsureProcess(std::wstring& failureOut) noexcept
        {
            if (client && client->is_connected())
            {
                return true;
            }
            const bool hadProcess = client != nullptr;
            Disconnect(true);
            if (hadProcess)
            {
                LogHarness(options.name, L"the process ended; starting a new one");
            }
            SetState(HarnessState::Starting);
            const auto started = std::chrono::steady_clock::now();
            const HarnessPermissionMode firstAsked = launchPermission.load();
            HarnessPermissionMode asked = firstAsked;
            std::wstring failure = StartOnce(asked);
            std::wstring firstFailure; // of the highest mode: the one that says what is not allowed
            while (!failure.empty())
            {
                const auto lower = NextHarnessPermissionMode(asked);
                if (!lower || !HarnessStartFailureNamesPermissionMode(failure))
                {
                    break; // not a refusal of the mode (or nothing lower exists): a real failure
                }
                LogHarness(options.name, std::wstring{ L"the start with permission mode " } + HarnessPermissionModeWord(asked) + L" was REFUSED (" + failure.substr(0, 300) + L"); starting again with " + HarnessPermissionModeWord(*lower));
                if (firstFailure.empty())
                {
                    firstFailure = std::move(failure);
                }
                asked = *lower;
                failure = StartOnce(asked);
            }
            if (!failure.empty())
            {
                // When every mode failed, the first text is the informative one (the later ones
                // repeat it, or name a mode the user never asked about). launchPermission is
                // left as it was: nothing proved that a lower mode works.
                if (!firstFailure.empty())
                {
                    failure = std::move(firstFailure);
                }
                LogHarness(options.name, L"start FAILED: " + failure);
                {
                    std::lock_guard lock{ mutex };
                    inventory = HarnessInventory{}; // no process answered: nothing is known
                }
                SetState(HarnessState::Failed, HarnessEventKind::Error, failure, true);
                failureOut = std::move(failure); // noexcept move: the caller reports it for a waiting message
                return false;
            }
            if (asked != firstAsked)
            {
                // A lower mode started where the higher one was refused: later starts of this
                // conversation ask for it directly, instead of failing once first.
                launchPermission.store(asked);
                LogHarness(options.name, std::wstring{ L"permission mode " } + HarnessPermissionModeWord(firstAsked) + L" is taken as NOT ALLOWED for this conversation; its later starts ask for " + HarnessPermissionModeWord(asked));
            }
            processIsFresh = true;
            readyAnnounced = false;
            lastSettledReport.clear();
            {
                std::lock_guard lock{ mutex };
                costAtProcessStart = record.usage.costUsd;
                inventory = HarnessInventory{}; // a new process: the old report is not its report
            }
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            LogHarness(options.name, L"process up pid=" + std::to_wstring(client->get_pid()) + L" in " + std::to_wstring(ms) + L" ms (model " + options.model + L")");
            // Before Ready, so that no message can reach a process that still runs in a mode
            // which can ask. The mode is in the `initialize` answer the SDK kept.
            std::wstring reported;
            try
            {
                if (const auto info = client->get_server_info(); info && info->is_object())
                {
                    // ⚠ Only this one key is read. The answer also holds the user's ACCOUNT
                    // object, which must never reach a log or the record.
                    if (const auto mode = info->find("current_permission_mode"); mode != info->end() && mode->is_string())
                    {
                        reported = HarnessFromUtf8(mode->get<std::string>());
                    }
                }
            }
            catch (...)
            {
                LogSwallowedException(L"HarnessConversation::EnsureProcess (initialize answer)");
            }
            SettlePermissionMode(asked, reported, L"at start");
            SetState(HarnessState::Ready);
            return true;
        }

        // -------------------------------------------------------------------------------
        // a turn
        // -------------------------------------------------------------------------------

        // Report the end of a turn and write the record.
        void FinishTurn(uint64_t turn, uint64_t turnEpoch, std::wstring text, bool isError, bool interrupted, int64_t durationMs, HarnessState next) noexcept
        {
            if (isError && !text.empty())
            {
                HarnessItem note;
                note.kind = HarnessItemKind::Notice;
                note.turn = turn;
                note.atUnixMs = NowUnixMs();
                note.text = text;
                note.isError = true;
                Record(turnEpoch, std::move(note));
            }
            {
                std::lock_guard lock{ mutex };
                // A reset already put the state where it belongs; a stale turn must not undo it.
                if (epoch == turnEpoch)
                {
                    state = next;
                }
            }
            activeTurn.store(0);
            HarnessEvent e;
            e.kind = HarnessEventKind::TurnCompleted;
            e.epoch = turnEpoch;
            e.turn = turn;
            e.text = std::move(text);
            e.isError = isError;
            e.interrupted = interrupted;
            e.durationMs = durationMs;
            Emit(std::move(e));
            Persist();
        }

        // Handle one `system` line of a turn.
        void OnSystemMessage(const claude::SystemMessage& message, uint64_t turn, uint64_t turnEpoch)
        {
            const HarnessSystemFacts facts = ClassifyHarnessSystemMessage(SdkJsonText(message.raw_json));
            switch (facts.kind)
            {
            case HarnessSystemKind::Init:
            {
                std::wstring knownMode;
                {
                    std::lock_guard lock{ mutex };
                    if (epoch == turnEpoch && !facts.model.empty())
                    {
                        record.model = facts.model;
                    }
                    knownMode = inventory.permissionMode;
                }
                // The permission mode, checked at EVERY turn's start (one string compare): it is
                // the process's own word, and it differs from what the harness holds when this
                // Claude Code reported no mode at start, or when something changed the mode since.
                // The policy then runs again, before the model's first tool call of this turn.
                std::wstring modeNow = knownMode;
                if (!facts.permissionMode.empty() && facts.permissionMode != knownMode)
                {
                    if (facts.permissionMode != lastSettledReport)
                    {
                        modeNow = SettlePermissionMode(launchPermission.load(), facts.permissionMode, readyAnnounced ? L"at a later turn" : L"at the first turn");
                    }
                    else
                    {
                        // The harness asked for another mode after exactly this report, Claude
                        // Code said yes, and the process still reports the old mode. Asking again
                        // cannot help; what is recorded must be what is true.
                        modeNow = facts.permissionMode;
                        {
                            std::lock_guard lock{ mutex };
                            inventory.permissionMode = modeNow;
                        }
                        LogHarness(options.name, L"permission mode: the process still reports '" + modeNow + L"' after the switch; that is the mode in effect");
                    }
                }
                if (readyAnnounced)
                {
                    break; // Claude Code repeats `init` at every turn; one report per process
                }
                readyAnnounced = true;
                {
                    // The process's own account of what its model can use (Inventory()).
                    std::lock_guard lock{ mutex };
                    inventory.known = true;
                    inventory.complete = true;
                    inventory.cliVersion = facts.cliVersion;
                    inventory.model = facts.model;
                    inventory.apiKeySource = facts.apiKeySource;
                    inventory.outputStyle = facts.outputStyle;
                    inventory.tools = facts.tools;
                    inventory.mcpServers = facts.mcpServers;
                    inventory.slashCommands = facts.slashCommands;
                    inventory.skills = facts.skills;
                    inventory.agents = facts.agents;
                    inventory.plugins = facts.plugins;
                }
                std::wstring servers;
                for (const auto& [name, status] : facts.mcpServers)
                {
                    servers += L" " + name + L"=" + status;
                    if (status != L"connected")
                    {
                        // The model then has no such tools, and would answer without them.
                        HarnessEvent warn;
                        warn.kind = HarnessEventKind::Notice;
                        warn.epoch = turnEpoch;
                        warn.turn = turn;
                        warn.isError = true;
                        warn.text = L"The tool server '" + name + L"' is not connected (" + status + L").";
                        Emit(std::move(warn));
                    }
                }
                std::wstring toolNames;
                for (size_t i = 0; i < facts.tools.size() && i < 16; ++i)
                {
                    toolNames += (i ? L"," : L"") + facts.tools[i];
                }
                // One line with the whole surface: a count for each kind, and the tools by name (a
                // tool the harness did not give is the first thing to look for here).
                LogHarness(options.name, L"init: claude " + facts.cliVersion + L" model=" + facts.model + L" permissionMode=" + facts.permissionMode + L" tools=" + std::to_wstring(facts.tools.size()) + L" [" + toolNames + L"] servers:" + servers + L" commands=" + std::to_wstring(facts.slashCommands.size()) + L" skills=" + std::to_wstring(facts.skills.size()) + L" agents=" + std::to_wstring(facts.agents.size()) + L" plugins=" + std::to_wstring(facts.plugins.size()) + L" apiKeySource=" + facts.apiKeySource);
                HarnessEvent e;
                e.kind = HarnessEventKind::Ready;
                e.epoch = turnEpoch;
                e.turn = turn;
                e.model = facts.model;
                e.cliVersion = facts.cliVersion;
                e.permissionMode = modeNow.empty() ? facts.permissionMode : modeNow;
                Emit(std::move(e));
                break;
            }
            case HarnessSystemKind::PermissionDenied:
                // Claude Code refused a tool call (the model got the refusal as that call's
                // result). In bypass this cannot happen; in auto it is the classifier's "no", in
                // dontAsk a tool that is not on the allowed list. The line is the only trace of
                // WHY a turn ended without the tool's work.
                LogHarness(options.name, L"turn " + std::to_wstring(turn) + L": permission DENIED for " + facts.deniedTool + L": " + facts.deniedReason.substr(0, 300));
                break;
            case HarnessSystemKind::Compacting:
            {
                HarnessEvent e;
                e.kind = HarnessEventKind::Compacting;
                e.epoch = turnEpoch;
                e.turn = turn;
                Emit(std::move(e));
                break;
            }
            case HarnessSystemKind::CompactBoundary:
            {
                {
                    std::lock_guard lock{ mutex };
                    if (epoch == turnEpoch)
                    {
                        ++record.compactions;
                        record.usage.contextTokens = facts.postTokens; // what the compaction left
                    }
                }
                HarnessItem note;
                note.kind = HarnessItemKind::Notice;
                note.turn = turn;
                note.atUnixMs = NowUnixMs();
                note.text = L"The context was compacted: " + std::to_wstring(facts.preTokens) + L" -> " + std::to_wstring(facts.postTokens) + L" tokens.";
                Record(turnEpoch, std::move(note));
                LogHarness(options.name, L"compacted (" + facts.compactTrigger + L"): " + std::to_wstring(facts.preTokens) + L" -> " + std::to_wstring(facts.postTokens) + L" tokens in " + std::to_wstring(facts.compactDurationMs) + L" ms");
                HarnessEvent e;
                e.kind = HarnessEventKind::Compacted;
                e.epoch = turnEpoch;
                e.turn = turn;
                e.preTokens = facts.preTokens;
                e.postTokens = facts.postTokens;
                e.durationMs = facts.compactDurationMs;
                Emit(std::move(e));
                break;
            }
            case HarnessSystemKind::CompactFinished:
                if (!facts.compactOk)
                {
                    LogHarness(options.name, L"a compaction did not succeed");
                }
                break;
            case HarnessSystemKind::Other:
            default:
                break;
            }
        }

        // Answer one user message. Worker thread only; a process is connected.
        void RunTurn(const Pending& job) noexcept
        {
            const auto started = std::chrono::steady_clock::now();
            const auto elapsedMs = [&started]() {
                return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
            };
            uint64_t turnEpoch = 0;
            try
            {
                std::wstring toSend = job.text;
                {
                    std::lock_guard lock{ mutex };
                    turnEpoch = epoch;
                    state = HarnessState::Running;
                    if (processIsFresh)
                    {
                        // The process knows nothing of the earlier turns: tell it. Built BEFORE
                        // this turn's own message joins the record.
                        toSend = ComposeColdStartMessage(BuildHarnessDigest(record, options.digestTurns, options.digestChars), job.text);
                    }
                    record.turns = (std::max)(record.turns, job.turn);
                }
                activeTurnEpoch.store(turnEpoch);
                activeTurn.store(job.turn);
                interruptRequested.store(false);

                HarnessItem user;
                user.kind = HarnessItemKind::User;
                user.turn = job.turn;
                user.atUnixMs = NowUnixMs();
                user.text = job.text;
                Record(turnEpoch, std::move(user));
                {
                    HarnessEvent e;
                    e.kind = HarnessEventKind::UserMessage;
                    e.epoch = turnEpoch;
                    e.turn = job.turn;
                    e.text = job.text;
                    Emit(std::move(e));
                }

                try
                {
                    client->send_query(HarnessToUtf8(toSend));
                }
                catch (const std::exception& ex)
                {
                    LogSwallowedException(L"HarnessConversation::RunTurn (send)");
                    const std::wstring why = L"The message could not be sent: " + HarnessFromUtf8(ex.what());
                    Disconnect(true);
                    FinishTurn(job.turn, turnEpoch, why, true, false, elapsedMs(), HarnessState::Failed);
                    return;
                }
                processIsFresh = false;

                claude::MessageStream stream = client->receive_messages();
                bool interruptSent = false;
                auto interruptAt = started;
                auto lastActivity = std::chrono::steady_clock::now();
                std::wstring lastText; // the newest assistant text, the fallback for an empty result
                std::string lastUsageMessageId;

                for (;;)
                {
                    bool abort = false; // a reset or a shutdown: the worker's loop ends the process
                    bool newer = false; // a newer message waits: newest wins
                    {
                        std::lock_guard lock{ mutex };
                        abort = stopRequested || resetRequested;
                        newer = pending.has_value();
                    }
                    if (abort)
                    {
                        FinishTurn(job.turn, turnEpoch, lastText, false, true, elapsedMs(), HarnessState::Ready);
                        return;
                    }
                    if ((newer || interruptRequested.load()) && !interruptSent)
                    {
                        interruptSent = true;
                        interruptAt = std::chrono::steady_clock::now();
                        try
                        {
                            client->interrupt(); // returns when Claude Code acknowledged it
                        }
                        catch (...)
                        {
                            // No acknowledgement (the process hangs or ended): end it. The next
                            // message starts a new process, which gets the digest.
                            LogSwallowedException(L"HarnessConversation::RunTurn (interrupt)");
                            Disconnect(true);
                            FinishTurn(job.turn, turnEpoch, lastText, false, true, elapsedMs(), HarnessState::Stopped);
                            return;
                        }
                    }

                    std::optional<claude::Message> message = stream.get_next_for(std::chrono::milliseconds{ 100 });
                    const auto now = std::chrono::steady_clock::now();
                    if (!message)
                    {
                        if (!stream.has_more())
                        {
                            // The stream stopped without a result: the process ended, or the
                            // SDK's reader failed.
                            std::wstring why = L"Claude Code ended during the turn.";
                            if (const std::wstring tail = StderrTailText(); !tail.empty())
                            {
                                why += L" (" + tail + L")";
                            }
                            LogHarness(options.name, L"turn " + std::to_wstring(job.turn) + L" FAILED: " + why);
                            Disconnect(true);
                            FinishTurn(job.turn, turnEpoch, why, !interruptSent, interruptSent, elapsedMs(), HarnessState::Failed);
                            return;
                        }
                        if (toolCallsInFlight.load() > 0)
                        {
                            lastActivity = now; // a tool is working: that is not silence
                        }
                        if (interruptSent && now - interruptAt > std::chrono::seconds{ 10 })
                        {
                            // Acknowledged, but no result line followed. A result that arrived
                            // late would be taken for the NEXT turn's, so the process goes.
                            LogHarness(options.name, L"turn " + std::to_wstring(job.turn) + L": no result 10 s after the interrupt; ending the process");
                            Disconnect(true);
                            FinishTurn(job.turn, turnEpoch, lastText, false, true, elapsedMs(), HarnessState::Stopped);
                            return;
                        }
                        if (options.turnSilenceTimeoutMs > 0 && now - lastActivity > std::chrono::milliseconds{ options.turnSilenceTimeoutMs })
                        {
                            const std::wstring why = L"Claude Code did not answer for " + std::to_wstring(options.turnSilenceTimeoutMs / 1000) + L" s; the process was ended.";
                            LogHarness(options.name, L"turn " + std::to_wstring(job.turn) + L" FAILED: " + why);
                            Disconnect(true);
                            FinishTurn(job.turn, turnEpoch, why, true, false, elapsedMs(), HarnessState::Failed);
                            return;
                        }
                        continue;
                    }
                    lastActivity = now;

                    if (const auto* assistant = std::get_if<claude::AssistantMessage>(&*message))
                    {
                        if (assistant->usage)
                        {
                            // Claude Code writes one line per content block and repeats the
                            // request's usage on each; the context size is the same on all of
                            // them, so taking it from every line is correct.
                            const HarnessRequestUsage u = ParseHarnessRequestUsage(SdkJsonText(*assistant->usage));
                            std::lock_guard lock{ mutex };
                            if (epoch == turnEpoch && u.Context() > 0)
                            {
                                record.usage.contextTokens = u.Context();
                            }
                        }
                        for (const auto& block : assistant->content)
                        {
                            const auto* textBlock = std::get_if<claude::TextBlock>(&block);
                            if (!textBlock || textBlock->text.empty())
                            {
                                continue; // tool_use blocks are reported by InvokeTool, with the answer
                            }
                            lastText = HarnessFromUtf8(textBlock->text);
                            HarnessItem item;
                            item.kind = HarnessItemKind::Assistant;
                            item.turn = job.turn;
                            item.atUnixMs = NowUnixMs();
                            item.text = lastText;
                            Record(turnEpoch, std::move(item));
                            HarnessEvent e;
                            e.kind = HarnessEventKind::AssistantText;
                            e.epoch = turnEpoch;
                            e.turn = job.turn;
                            e.text = lastText;
                            Emit(std::move(e));
                        }
                        continue;
                    }
                    if (const auto* system = std::get_if<claude::SystemMessage>(&*message))
                    {
                        OnSystemMessage(*system, job.turn, turnEpoch);
                        continue;
                    }
                    if (const auto* partial = std::get_if<claude::StreamEvent>(&*message))
                    {
                        if (options.partialMessages && partial->event == "content_block_delta" && partial->data.is_object())
                        {
                            const auto delta = partial->data.find("delta");
                            if (delta != partial->data.end() && delta->is_object() && delta->value("type", std::string{}) == "text_delta")
                            {
                                HarnessEvent e;
                                e.kind = HarnessEventKind::AssistantDelta;
                                e.epoch = turnEpoch;
                                e.turn = job.turn;
                                e.text = HarnessFromUtf8(delta->value("text", std::string{}));
                                Emit(std::move(e));
                            }
                        }
                        continue;
                    }
                    if (const auto* result = std::get_if<claude::ResultMessage>(&*message))
                    {
                        const HarnessResultFacts facts = ParseHarnessResultMessage(SdkJsonText(result->raw_json));
                        {
                            std::lock_guard lock{ mutex };
                            if (epoch == turnEpoch)
                            {
                                // The result's usage is the turn's total (every request of it).
                                const auto& u = result->result.usage;
                                record.usage.inputTokens += u.input_tokens;
                                record.usage.outputTokens += u.output_tokens;
                                record.usage.cacheReadTokens += u.cache_read_input_tokens;
                                record.usage.cacheCreationTokens += u.cache_creation_input_tokens;
                                // total_cost_usd counts from the start of THIS process.
                                record.usage.costUsd = costAtProcessStart + facts.totalCostUsd;
                                // The window the conversation may fill: the compaction window when
                                // one is set, else the model's.
                                const int64_t compactAt = ClampHarnessAutoCompactTokens(options.autoCompactTokens);
                                const int64_t window = (compactAt != 0 && facts.contextWindow != 0) ? (std::min)(compactAt, facts.contextWindow) : (compactAt != 0 ? compactAt : facts.contextWindow);
                                if (window != 0)
                                {
                                    record.usage.contextWindow = window;
                                }
                            }
                        }
                        std::wstring text = !facts.text.empty() ? facts.text : lastText;
                        // An interrupted turn ends with an error-shaped result; that is the
                        // interrupt working, not a failure.
                        const bool failed = facts.isError && !interruptSent;
                        if (failed && text.empty())
                        {
                            text = !facts.errors.empty() ? facts.errors.front() : (L"The turn failed (" + facts.subtype + L").");
                        }
                        LogHarness(options.name, L"turn " + std::to_wstring(job.turn) + (failed ? L" FAILED" : (interruptSent ? L" interrupted" : L" done")) + L": " + std::to_wstring(elapsedMs()) + L" ms, " + std::to_wstring(facts.numTurns) + L" request(s)" + (failed ? L": " + text.substr(0, 300) : std::wstring{}));
                        FinishTurn(job.turn, turnEpoch, std::move(text), failed, interruptSent, elapsedMs(), HarnessState::Ready);
                        return;
                    }
                    // A user line (a tool result's echo) or a rate-limit line: nothing to report.
                }
            }
            catch (...)
            {
                LogSwallowedException(L"HarnessConversation::RunTurn");
                Disconnect(true);
                FinishTurn(job.turn, turnEpoch != 0 ? turnEpoch : Epoch(), L"The turn failed inside Agentmaster; see hooks.log.", true, false, elapsedMs(), HarnessState::Failed);
            }
        }

        uint64_t Epoch() const
        {
            std::lock_guard lock{ mutex };
            return epoch;
        }

        // -------------------------------------------------------------------------------
        // the worker
        // -------------------------------------------------------------------------------

        // The worker thread: waits for something to do, then starts a process, answers a
        // message, or carries out a reset. Ends at Shutdown.
        void Run() noexcept
        {
            for (;;)
            {
                std::optional<Pending> job;
                bool doReset = false;
                bool doPark = false;
                {
                    std::unique_lock lock{ mutex };
                    // `client` is written by this thread only, so reading it here is safe.
                    const auto hasWork = [this] { return stopRequested || resetRequested || pending.has_value() || (wantProcess && !client); };
                    while (!hasWork())
                    {
                        if (parkAt && client)
                        {
                            // A park is pending: sleep until its time, or until something to do
                            // arrives (which cancels it — Prewarm / Send / Reset clear parkAt).
                            if (std::chrono::steady_clock::now() >= *parkAt)
                            {
                                parkAt.reset();
                                doPark = true;
                                break;
                            }
                            wake.wait_until(lock, *parkAt);
                        }
                        else
                        {
                            parkAt.reset(); // no process: there is nothing to park
                            wake.wait(lock);
                        }
                    }
                    if (stopRequested)
                    {
                        break;
                    }
                    if (doPark)
                    {
                        // Handled below, outside the lock. Nothing else is taken: hasWork() was
                        // false when the park was decided.
                    }
                    else if (resetRequested)
                    {
                        resetRequested = false;
                        doReset = true;
                    }
                    else if (pending)
                    {
                        job = std::move(pending);
                        pending.reset();
                        wantProcess = false;
                    }
                    else
                    {
                        wantProcess = false; // one start attempt per request: a failure must not loop
                    }
                }
                if (doPark)
                {
                    // The process is idle and nobody uses the conversation: end it. The record
                    // stays, so the next process starts with its digest (RunTurn, processIsFresh).
                    Disconnect(!options.claudePersistsSession);
                    LogHarness(options.name, L"parked: the idle process was ended (the conversation continues from its record)");
                    SetState(HarnessState::Stopped);
                    continue;
                }
                if (doReset)
                {
                    // Reset() already raised the epoch and emptied the record; what is left is the
                    // process and the file.
                    Disconnect(true);
                    if (!recordPath.empty())
                    {
                        DeleteHarnessRecordAt(recordPath);
                    }
                    LogHarness(options.name, L"reset (epoch " + std::to_wstring(Epoch()) + L")");
                    SetState(HarnessState::Stopped);
                    continue; // Reset() set wantProcess when a process was up: the wait above starts one
                }
                std::wstring startFailure;
                if (!EnsureProcess(startFailure))
                {
                    if (job)
                    {
                        // The message that waited for this process ends here, with the reason.
                        // It still joins the record, so the history shows what was asked.
                        const uint64_t turnEpoch = Epoch();
                        {
                            std::lock_guard lock{ mutex };
                            if (epoch == turnEpoch)
                            {
                                record.turns = (std::max)(record.turns, job->turn);
                            }
                        }
                        HarnessItem user;
                        user.kind = HarnessItemKind::User;
                        user.turn = job->turn;
                        user.atUnixMs = NowUnixMs();
                        user.text = job->text;
                        Record(turnEpoch, std::move(user));
                        HarnessEvent e;
                        e.kind = HarnessEventKind::UserMessage;
                        e.epoch = turnEpoch;
                        e.turn = job->turn;
                        e.text = job->text;
                        Emit(std::move(e));
                        FinishTurn(job->turn, turnEpoch, std::move(startFailure), true, false, 0, HarnessState::Failed);
                    }
                    continue;
                }
                if (job)
                {
                    RunTurn(*job);
                }
            }
            // A child that keeps its own transcript gets the time to write it; any other is ended.
            Disconnect(!options.claudePersistsSession);
            {
                std::lock_guard lock{ mutex };
                state = HarnessState::Stopped;
            }
        }
    };

    HarnessConversation::HarnessConversation(HarnessOptions options, EventSink sink) :
        _impl{ std::make_unique<Impl>() }
    {
        if (!IsValidHarnessName(options.name))
        {
            throw std::invalid_argument("HarnessOptions::name must be 1 to 48 characters of a-z, 0-9 and '-'.");
        }
        for (const auto& server : options.toolServers)
        {
            if (!IsValidToolName(server.name))
            {
                throw std::invalid_argument("A harness tool server needs a name of letters, digits, '_' and '-'.");
            }
            for (const auto& tool : server.tools)
            {
                if (!IsValidToolName(tool.name))
                {
                    throw std::invalid_argument("A harness tool needs a name of letters, digits, '_' and '-'.");
                }
            }
        }
        Impl& s = *_impl;
        s.options = std::move(options);
        s.sink = std::move(sink);
        s.workDir = !s.options.workDir.empty() ? s.options.workDir : (HarnessStateDir() + L"\\" + s.options.name);
        s.promptPath = s.workDir + L"\\system-prompt.md";
        if (s.options.persistRecord)
        {
            s.recordPath = !s.options.recordPath.empty() ? s.options.recordPath : HarnessRecordPathIn(HarnessStateDir(), s.options.name);
            s.record = LoadHarnessRecordFrom(s.recordPath);
        }
        s.record.name = s.options.name;
        s.nextTurn = s.record.turns + 1; // turn ids go on where the stored conversation stopped
        // Last: the thread reads everything set above.
        s.worker = std::thread{ [impl = _impl.get()] { impl->Run(); } };
    }

    HarnessConversation::~HarnessConversation()
    {
        Shutdown();
    }

    void HarnessConversation::Prewarm()
    {
        {
            std::lock_guard lock{ _impl->mutex };
            if (_impl->stopRequested)
            {
                return;
            }
            _impl->wantProcess = true;
            _impl->parkAt.reset(); // somebody is back: the process stays
        }
        _impl->wake.notify_all();
    }

    void HarnessConversation::ParkAfter(int64_t delayMs)
    {
        {
            std::lock_guard lock{ _impl->mutex };
            if (_impl->stopRequested)
            {
                return;
            }
            _impl->parkAt = std::chrono::steady_clock::now() + std::chrono::milliseconds{ (std::max)(int64_t{ 0 }, delayMs) };
            // A Prewarm that did not start its process yet would start one only for the park to
            // end it: drop the request.
            _impl->wantProcess = false;
        }
        _impl->wake.notify_all(); // the worker takes the new deadline
    }

    uint64_t HarnessConversation::Send(std::wstring text)
    {
        if (text.find_first_not_of(L" \t\r\n") == std::wstring::npos)
        {
            return 0; // nothing to ask
        }
        uint64_t turn = 0;
        std::optional<Impl::Pending> replaced;
        uint64_t epoch = 0;
        {
            std::lock_guard lock{ _impl->mutex };
            if (_impl->stopRequested)
            {
                return 0;
            }
            replaced = std::move(_impl->pending); // newest wins: an unstarted message is dropped
            turn = _impl->nextTurn++;
            _impl->pending = Impl::Pending{ turn, std::move(text) };
            _impl->parkAt.reset(); // a request: the conversation is in use
            epoch = _impl->epoch;
        }
        _impl->wake.notify_all();
        if (replaced)
        {
            // Its sender waits for an end: tell it, with nothing answered.
            HarnessEvent e;
            e.kind = HarnessEventKind::TurnCompleted;
            e.epoch = epoch;
            e.turn = replaced->turn;
            e.interrupted = true;
            _impl->Emit(std::move(e));
        }
        return turn;
    }

    void HarnessConversation::Interrupt()
    {
        _impl->interruptRequested.store(true); // the turn loop reads it within 100 ms; without a turn it is cleared at the next one
    }

    void HarnessConversation::Reset()
    {
        std::optional<Impl::Pending> dropped;
        uint64_t oldEpoch = 0;
        {
            std::lock_guard lock{ _impl->mutex };
            if (_impl->stopRequested)
            {
                return;
            }
            oldEpoch = _impl->epoch;
            ++_impl->epoch; // from here on, everything of the old conversation is stale
            dropped = std::move(_impl->pending);
            _impl->pending.reset();
            const std::wstring name = _impl->record.name;
            _impl->record = HarnessRecord{};
            _impl->record.name = name;
            _impl->nextTurn = 1;
            _impl->parkAt.reset(); // a reset comes from a user who is there
            _impl->resetRequested = true;
            // Keep a process ready when one was in use: the user who resets is about to ask again.
            _impl->wantProcess = _impl->state != HarnessState::Stopped;
        }
        _impl->wake.notify_all();
        if (dropped)
        {
            HarnessEvent e;
            e.kind = HarnessEventKind::TurnCompleted;
            e.epoch = oldEpoch;
            e.turn = dropped->turn;
            e.interrupted = true;
            _impl->Emit(std::move(e));
        }
    }

    void HarnessConversation::Shutdown() noexcept
    {
        try
        {
            {
                std::lock_guard lock{ _impl->mutex };
                _impl->stopRequested = true;
            }
            _impl->wake.notify_all();
            if (_impl->worker.joinable())
            {
                _impl->worker.join();
            }
        }
        catch (...)
        {
            LogSwallowedException(L"HarnessConversation::Shutdown");
        }
    }

    HarnessState HarnessConversation::State() const
    {
        std::lock_guard lock{ _impl->mutex };
        return _impl->state;
    }

    uint64_t HarnessConversation::Epoch() const
    {
        return _impl->Epoch();
    }

    HarnessRecord HarnessConversation::Record() const
    {
        std::lock_guard lock{ _impl->mutex };
        return _impl->record;
    }

    HarnessInventory HarnessConversation::Inventory() const
    {
        std::lock_guard lock{ _impl->mutex };
        return _impl->inventory;
    }

    std::vector<std::wstring> HarnessConversation::DescribeCommandLine() const
    {
        // A transport that is never connected: build_command only reads the options.
        const claude::internal::SubprocessTransport transport{ _impl->BuildSdkOptions("claude.exe", _impl->launchPermission.load()) };
        std::vector<std::wstring> out;
        for (const auto& arg : transport.build_command())
        {
            out.push_back(HarnessFromUtf8(arg));
        }
        return out;
    }
}
