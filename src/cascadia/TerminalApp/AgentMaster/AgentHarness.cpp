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
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>
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

    // The answer to one control request of the harness's own (WatchedTransport::Request).
    struct TapAnswer
    {
        bool answered{}; // false: no answer came within the time limit, or the process ended
        bool ok{}; // the answer's subtype was "success"
        std::string error; // Claude Code's error text when the answer was not "success"
        SdkJson response; // the answer's response object; null when it carried none
    };

    // Wraps the SDK's subprocess transport for two things the SDK client cannot do.
    //
    // 1. Report the child's EXIT. The SDK client's reader loop stops quietly when the transport
    //    is no longer running, and leaves every control request that waits for an answer waiting —
    //    `initialize` for 60 s when Claude Code ends at once (an option it does not know, a broken
    //    install), `interrupt` the same. This wrapper keeps the loop alive until the transport's
    //    queue is drained, then THROWS from read_messages: the client's loop catches that, stops
    //    its message queue and fails the pending requests at once.
    //
    // 2. Send CONTROL REQUESTS of the harness's own (Request): set_max_thinking_tokens,
    //    apply_flag_settings and get_settings — the thinking policy of AgentHarnessCore.h. The
    //    client's control protocol is private, and its public calls cover set_permission_mode,
    //    set_model and interrupt only. A request goes out through the transport's write (which
    //    takes the transport's write lock, so it never interleaves with the client's own lines),
    //    and its answer is taken OUT of the stream in read_messages, before the client's reader
    //    sees it: the client would find no waiting request for the id and drop it.
    //
    // Everything else is forwarded unchanged.
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
            TakeOwnAnswers(messages);
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
            {
                // Under the tap's lock, so a Request that is just about to wait cannot miss the
                // wake-up below: it checks the flag under the same lock.
                std::lock_guard lock{ _tapMutex };
                _exitReported.store(true);
            }
            _tapAnswered.notify_all(); // a Request that waits for an answer returns at once
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

        // Send one control request of the harness's own and wait for Claude Code's answer.
        //   request — the request object, e.g. {"subtype": "set_max_thinking_tokens",
        //             "max_thinking_tokens": 0}. It is sent as the `request` of a control_request
        //             line, under an id of its own ("am_ctl_<n>").
        //   timeout — how long to wait for the answer.
        //   returns — the answer; `answered` false when none came in time or the process ended
        //             (a late answer is then dropped, as the id no longer waits).
        //   throws  — what the transport's write throws (the process is gone: claude::ClaudeError
        //             and its kin); std::bad_alloc.
        // Call it from one thread at a time per request (the worker does); it may run while the
        // SDK's reader thread reads, which is where the answer arrives.
        TapAnswer Request(SdkJson request, std::chrono::milliseconds timeout)
        {
            const std::string id = std::string{ kTapPrefix } + std::to_string(_tapCounter.fetch_add(1) + 1);
            {
                std::lock_guard lock{ _tapMutex };
                _tapWaiting.insert(id); // BEFORE the write: an answer can arrive before write returns
            }
            const SdkJson message = { { "type", "control_request" }, { "request_id", id }, { "request", std::move(request) } };
            try
            {
                _inner->write(message.dump(-1, ' ', false, SdkJson::error_handler_t::replace) + "\n");
            }
            catch (...)
            {
                // The id must not wait forever in the set; the caller reports the failure.
                std::lock_guard lock{ _tapMutex };
                _tapWaiting.erase(id);
                throw;
            }
            std::unique_lock lock{ _tapMutex };
            _tapAnswered.wait_for(lock, timeout, [this, &id] { return _tapAnswers.count(id) != 0 || _exitReported.load(); });
            TapAnswer answer;
            if (const auto it = _tapAnswers.find(id); it != _tapAnswers.end())
            {
                answer = std::move(it->second);
                _tapAnswers.erase(it);
            }
            _tapWaiting.erase(id);
            return answer;
        }

    private:
        // The ids of the harness's own requests start with this; the SDK's own are "req_<n>_<hex>".
        static constexpr std::string_view kTapPrefix{ "am_ctl_" };

        // Take the answers to the harness's own requests out of `messages` and hand each to its
        // waiting Request. The SDK client's reader thread only (it is the caller of read_messages).
        //   messages — one read's messages; the answers of ours are erased from it, everything
        //              else keeps its order.
        //   throws   — std::bad_alloc (the answer's copy). The client's loop treats a throw like
        //              the end of the process, so a failure here costs the process, not a hang.
        void TakeOwnAnswers(std::vector<claude::Message>& messages)
        {
            const auto ours = [this](const claude::Message& m) {
                const auto* control = std::get_if<claude::protocol::ControlResponse>(&m);
                if (!control || control->response.request_id.rfind(kTapPrefix, 0) != 0)
                {
                    return false;
                }
                TapAnswer answer;
                answer.answered = true;
                answer.ok = control->response.subtype == "success";
                answer.error = control->response.error;
                answer.response = control->response.response;
                {
                    std::lock_guard lock{ _tapMutex };
                    // An answer that nobody waits for any more (its Request timed out) is dropped.
                    if (_tapWaiting.count(control->response.request_id) != 0)
                    {
                        _tapAnswers[control->response.request_id] = std::move(answer);
                    }
                }
                _tapAnswered.notify_all();
                return true; // never passed on: the client has no request with this id
            };
            messages.erase(std::remove_if(messages.begin(), messages.end(), ours), messages.end());
        }

        static constexpr int kDrainReads = 5;
        std::unique_ptr<claude::Transport> _inner;
        int _emptyReadsAfterExit{ 0 }; // the client's reader thread only
        std::atomic<bool> _exitReported{ false };

        // ---- the control tap; `_tapMutex` guards the two containers ----
        std::atomic<uint64_t> _tapCounter{ 0 };
        std::mutex _tapMutex;
        std::condition_variable _tapAnswered; // raised when an answer arrived or the process ended
        std::set<std::string> _tapWaiting; // ids of requests whose Request still waits
        std::map<std::string, TapAnswer> _tapAnswers; // answers that arrived, until Request takes them
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
        // The reasoning the conversation WANTS: HarnessOptions' start values, then whatever
        // SetThinking set. The worker reads both before every process start (they decide the
        // command line) and before every message (ApplyThinkingLive).
        HarnessThinking wantThinking{ HarnessThinking::Disabled };
        std::wstring wantEffort;

        // ---- cross-thread flags ----
        // The permission mode the NEXT start asks for. Bypass, until a start with it was refused
        // with an error AND a start with a lower mode then worked: from then on the lower mode,
        // for this object's life (what refuses a mode is a policy, and a policy does not change
        // between two starts; a new app run asks for bypass again). Written by the worker only;
        // atomic because DescribeCommandLine reads it from other threads.
        std::atomic<HarnessPermissionMode> launchPermission{ HarnessPermissionMode::Bypass };
        // True while a process wanted with thinking OFF or adaptive is started with
        // `--thinking adaptive` and switched live (the thinking policy). False, for this object's
        // life, once Claude Code refused or did not answer a live thinking switch: later starts
        // pass the literal --thinking value, and a later thinking change restarts the process.
        // Written by the worker only; atomic because DescribeCommandLine reads it from other threads.
        // A refused or unanswered live change of CLAUDE_CODE_EXTRA_BODY clears it too: the start
        // then carries the literal thinking AND the matching request body.
        std::atomic<bool> liveThinking{ true };
        // True when the model refuses "disabled", so that an "off" level needs the API's
        // between_tools in the request body (HarnessModelRefusesDisabledThinking, the thinking
        // policy). Set by the constructor from options.model, then by the worker from the
        // resolved id each process reports (an alias such as "sonnet" names no version). Atomic
        // because DescribeCommandLine reads it from other threads.
        std::atomic<bool> modelRefusesDisabledThinking{ true };
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
        // The client's transport wrapper: owned by `client` (null when no client exists), and the
        // way the worker sends its own control requests (WatchedTransport::Request).
        WatchedTransport* tap{ nullptr };
        // The reasoning of the running process: what it was STARTED with (its --thinking value;
        // CliDefault = none) and what it runs with NOW, after the harness's live changes. Valid
        // while `client` exists.
        HarnessThinking launchedThinking{ HarnessThinking::CliDefault };
        HarnessThinking appliedThinking{ HarnessThinking::CliDefault };
        // The request body of the running process carries the API's between_tools (its
        // CLAUDE_CODE_EXTRA_BODY, from the start's --settings or the last live patch).
        bool appliedBetweenTools{ false };
        std::wstring appliedEffort; // the effort word the harness last set; empty = none was passed (Claude Code's default)
        // False, for this object's life, once Claude Code refused or did not answer a live effort
        // change (apply_flag_settings): a later effort change then restarts the process.
        bool liveEffort{ true };
        // Claude Code's estimate of the running turn's thinking tokens: the deltas of its
        // system/thinking_tokens lines, summed (OnSystemMessage). Reset at each turn's start.
        int64_t turnThinkingTokens{ 0 };
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

        // The --thinking value a start passes for the wanted `thinking` (the thinking policy,
        // AgentHarnessCore.h). While live switching works, both Disabled and Adaptive start as
        // Adaptive: an explicit adaptive start config is Claude Code's reset anchor, the one way
        // back from "off" to adaptive without a restart; ApplyThinkingLive then turns an unwanted
        // thinking off before the first message. After a refused live switch (liveThinking false)
        // the literal value is passed, and CliDefault always passes none.
        HarnessThinking LaunchThinkingFor(HarnessThinking thinking) const noexcept
        {
            if (thinking == HarnessThinking::CliDefault)
            {
                return HarnessThinking::CliDefault;
            }
            return liveThinking.load() ? HarnessThinking::Adaptive : thinking;
        }

        // Whether a start for the wanted `thinking` carries the API's between_tools in its
        // request body (CLAUDE_CODE_EXTRA_BODY in the --settings env): only a start whose
        // --thinking is the literal "disabled" (live switching was refused) on a model that
        // refuses "disabled". A start that is switched off live gets the body live too
        // (ApplyThinkingLive), after Claude Code stopped thinking: the API refuses the body next
        // to Claude Code's adaptive request.
        bool LaunchBetweenToolsFor(HarnessThinking thinking) const noexcept
        {
            return LaunchThinkingFor(thinking) == HarnessThinking::Disabled && modelRefusesDisabledThinking.load();
        }

        // The --settings spec of this conversation, with the request body for `betweenTools`. One
        // place for the start (BuildSdkOptions) and the live patch (SendRequestBody), so the env
        // object a patch writes is the one the start wrote, with one variable changed.
        //   throws — std::bad_alloc.
        HarnessSettingsSpec SettingsSpecFor(bool betweenTools) const
        {
            HarnessSettingsSpec settings;
            settings.autoCompactTokens = options.autoCompactTokens;
            settings.apiMaxRetries = options.apiMaxRetries;
            settings.disableBypassPermissions = options.permissionProbe.bypassDisabledBySettings; // test seam
            settings.disableAutoMode = options.permissionProbe.autoDisabledBySettings; // test seam
            settings.extraRequestBody = betweenTools ? std::wstring{ kHarnessBetweenToolsRequestBody } : std::wstring{};
            return settings;
        }

        // The SDK options of this conversation.
        //   cliPath    — the narrow claude.exe path (NarrowPathForSdk); only stored, never opened.
        //   permission — the permission mode this start asks for (launchPermission, or the next
        //                lower mode after a refused start).
        //   thinking   — the thinking the conversation wants now (wantThinking); the --thinking
        //                value is LaunchThinkingFor(thinking), which may differ (the policy).
        //   effort     — the effort the conversation wants now (wantEffort); passed as --effort
        //                when it is one of Claude Code's words, else not at all.
        //   throws     — std::invalid_argument when a tool's inputSchemaJson is not a JSON object;
        //                std::bad_alloc.
        claude::ClaudeOptions BuildSdkOptions(const std::string& cliPath, HarnessPermissionMode permission, HarnessThinking thinking, const std::wstring& effort)
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
            o.settings = HarnessToUtf8(BuildHarnessSettingsJson(SettingsSpecFor(LaunchBetweenToolsFor(thinking))));

            // The whole parent environment, minus the variables of a parent session or tab.
            o.sanitize_environment = false;
            o.inherit_environment = true;
            o.environment = BuildHarnessEnvironment(options.name);

            switch (LaunchThinkingFor(thinking))
            {
            case HarnessThinking::Disabled:
                o.thinking = claude::ThinkingConfig{ claude::ThinkingConfigDisabled{} }; // --thinking disabled (only after a refused live switch)
                break;
            case HarnessThinking::Adaptive:
                o.thinking = claude::ThinkingConfig{ claude::ThinkingConfigAdaptive{} }; // --thinking adaptive
                break;
            case HarnessThinking::CliDefault:
            default:
                break;
            }
            // The effort rides this flag alone: no CLAUDE_CODE_EFFORT_LEVEL anywhere in the child
            // (BuildHarnessSettingsJson, BuildHarnessEnvironment), so nothing overrides it or a
            // live change of it later.
            if (IsHarnessEffortWord(effort))
            {
                o.effort = HarnessToUtf8(effort);
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
            tap = nullptr; // the transport wrapper dies with the client, just below
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
                        // The reasoning wanted NOW: a SetThinking that came while no process was
                        // up is on this start's command line.
                        HarnessThinking thinking = HarnessThinking::Disabled;
                        std::wstring effort;
                        {
                            std::lock_guard lock{ mutex };
                            thinking = wantThinking;
                            effort = wantEffort;
                        }
                        const claude::ClaudeOptions sdkOptions = BuildSdkOptions(*cliPath, permission, thinking, effort);
                        auto watched = std::make_unique<WatchedTransport>(claude::create_subprocess_transport(sdkOptions));
                        WatchedTransport* const watchedRaw = watched.get();
                        client = std::make_unique<claude::ClaudeClient>(sdkOptions, std::move(watched));
                        tap = watchedRaw; // owned by the client from here on; Disconnect clears it
                        client->connect(); // starts claude.exe and waits for its `initialize` answer
                        if (!client->is_connected())
                        {
                            failure = L"Claude Code ended while it started.";
                        }
                        else
                        {
                            // What this process was started with: ApplyThinkingLive compares the
                            // wanted reasoning with these.
                            launchedThinking = LaunchThinkingFor(thinking);
                            appliedThinking = launchedThinking;
                            // The same read BuildSdkOptions made above: only this worker writes
                            // the atomic it depends on.
                            appliedBetweenTools = LaunchBetweenToolsFor(thinking);
                            appliedEffort = IsHarnessEffortWord(effort) ? effort : std::wstring{};
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

        // How long the worker waits for Claude Code's answer to one of its own control requests.
        // Between turns Claude Code answers them at once (milliseconds, measured on 2.1.291); the
        // limit only guards against a process that stopped reading its input.
        static constexpr std::chrono::milliseconds kTapTimeout{ 5000 };

        // The word the inventory and hooks.log use for the thinking of a process
        // (HarnessInventory::thinking).
        //   thinking     — Claude Code's own thinking config, as the harness last set it.
        //   betweenTools — the request body carries the API's between_tools (thinking off on a
        //                  model that refuses "disabled").
        //   returns      — "adaptive"; "between_tools" or "disabled" for the two ways thinking is
        //                  off; "default" for a process left to Claude Code's own default.
        static const wchar_t* ThinkingWord(HarnessThinking thinking, bool betweenTools) noexcept
        {
            switch (thinking)
            {
            case HarnessThinking::Adaptive:
                return L"adaptive";
            case HarnessThinking::Disabled:
                return betweenTools ? L"between_tools" : L"disabled";
            case HarnessThinking::CliDefault:
            default:
                return L"default";
            }
        }

        // Change the request body of the RUNNING process live: apply_flag_settings with the
        // COMPLETE env object of its --settings (BuildHarnessEnvPatchJson), whose
        // CLAUDE_CODE_EXTRA_BODY is the API's between_tools or empty. Worker thread only.
        //   betweenTools — the body wanted.
        //   where        — "at start" or "before turn N", for the log line of a refusal.
        //   returns      — true when Claude Code took it (appliedBetweenTools then follows); false
        //                  when it refused or did not answer: liveThinking is cleared (later starts
        //                  carry the literal thinking and the matching body) and the caller starts
        //                  a new process.
        // Never throws.
        bool SendRequestBody(bool betweenTools, const std::wstring& where) noexcept
        {
            try
            {
                if (!tap)
                {
                    return false;
                }
                // The text is the harness's own (BuildHarnessEnvPatchJson): it parses; a failure
                // would throw into the catch below and count as a refusal.
                SdkJson settings = SdkJson::parse(HarnessToUtf8(BuildHarnessEnvPatchJson(SettingsSpecFor(betweenTools))));
                const TapAnswer answer = tap->Request(SdkJson{ { "subtype", "apply_flag_settings" }, { "settings", std::move(settings) } }, kTapTimeout);
                if (!answer.answered || !answer.ok)
                {
                    liveThinking.store(false);
                    LogHarness(options.name, std::wstring{ L"thinking: the live change of the request body to " } + (betweenTools ? L"between_tools" : L"none") + L" " + where + L" was " + (answer.answered ? L"REFUSED (" + HarnessFromUtf8(answer.error).substr(0, 200) + L")" : std::wstring{ L"NOT ANSWERED" }) + L"; a new process is started with the wanted values on its command line");
                    return false;
                }
                appliedBetweenTools = betweenTools;
                return true;
            }
            catch (...)
            {
                LogSwallowedException(L"HarnessConversation::SendRequestBody");
                liveThinking.store(false);
                return false;
            }
        }

        // The effort IN EFFECT, as the running process reports it: get_settings' applied.effort,
        // which is after Claude Code's own caps and the model's step-down of a level it does not
        // support. Worker thread only.
        //   returns — the effort word; empty when Claude Code reports none (a model without
        //             effort), did not answer, or no transport exists.
        // Never throws. Only applied.effort is read: the answer also carries the settings and
        // their sources, which must not reach a log.
        std::wstring ReadEffortInEffect() noexcept
        {
            try
            {
                if (!tap)
                {
                    return {};
                }
                const TapAnswer answer = tap->Request(SdkJson{ { "subtype", "get_settings" } }, kTapTimeout);
                if (!answer.answered || !answer.ok || !answer.response.is_object())
                {
                    return {};
                }
                const auto applied = answer.response.find("applied");
                if (applied == answer.response.end() || !applied->is_object())
                {
                    return {};
                }
                const auto effort = applied->find("effort");
                return (effort != applied->end() && effort->is_string()) ? HarnessFromUtf8(effort->get<std::string>()) : std::wstring{};
            }
            catch (...)
            {
                LogSwallowedException(L"HarnessConversation::ReadEffortInEffect");
                return {};
            }
        }

        // Bring the RUNNING process to the reasoning the conversation wants (the thinking policy,
        // AgentHarnessCore.h): set_max_thinking_tokens for Claude Code's thinking, apply_flag_settings
        // with the env object for the request body (the API's between_tools: "off" on a model that
        // refuses "disabled"), apply_flag_settings for the effort, then get_settings for the effort
        // in effect (Inventory().effort). The requests go in the order HarnessThinkingChange
        // documents, so the pair the API refuses (Claude Code adaptive + between_tools) never
        // exists between two of them. Worker thread only; `client` is connected.
        //   where   — "at start" or "before turn N", for the log line. "at start" also reads and
        //             logs the reasoning when nothing has to change, so hooks.log shows each
        //             process's reasoning once.
        //   returns — true when the process now runs as wanted (also when nothing had to change);
        //             false when it cannot be changed live: the change needs another --thinking
        //             start value (CliDefault on one side only, or a process that was not started
        //             adaptive), or Claude Code refused a request or did not answer it. The caller
        //             then starts a new process, whose command line carries the wanted values.
        // Never throws.
        bool ApplyThinkingLive(const std::wstring& where) noexcept
        {
            try
            {
                HarnessThinking thinking = HarnessThinking::Disabled;
                std::wstring effort;
                {
                    std::lock_guard lock{ mutex };
                    thinking = wantThinking;
                    effort = wantEffort;
                }
                const bool atStart = where == L"at start";
                // CliDefault can be neither reached nor left by a control request: only a start
                // without --thinking gives it.
                if ((thinking == HarnessThinking::CliDefault) != (appliedThinking == HarnessThinking::CliDefault))
                {
                    return false;
                }
                // CliDefault on both sides compares as "thinks" on both: no thinking request, only
                // an effort change can follow.
                HarnessReasoningState applied;
                applied.thinks = appliedThinking != HarnessThinking::Disabled;
                applied.betweenTools = appliedBetweenTools;
                applied.effort = appliedEffort;
                HarnessReasoningState wanted;
                wanted.thinks = thinking != HarnessThinking::Disabled;
                // "Off" on a model that refuses "disabled" is the API's between_tools (read from
                // the resolved id OnProcessUp stored; the policy).
                wanted.betweenTools = !wanted.thinks && modelRefusesDisabledThinking.load();
                wanted.effort = effort;
                const HarnessThinkingChange change = PlanHarnessThinkingChange(applied, wanted);
                if (change.Empty() && !atStart)
                {
                    return true; // the common case before a turn: nothing changed since the last one
                }
                const bool thinkingMoves = change.thinkingOff || change.thinkingOn || change.betweenToolsOn || change.betweenToolsOff;
                if (thinkingMoves && (launchedThinking != HarnessThinking::Adaptive || !liveThinking.load()))
                {
                    return false; // no adaptive reset anchor in this process: only a new start can change its thinking
                }
                if (!change.effort.empty() && !liveEffort)
                {
                    return false;
                }
                if (!tap && !change.Empty())
                {
                    return false;
                }
                // Toward "on": the body goes FIRST (the API refuses it next to Claude Code's
                // adaptive request).
                if (change.betweenToolsOff && !SendRequestBody(false, where))
                {
                    return false;
                }
                if (change.thinkingOff || change.thinkingOn)
                {
                    SdkJson request{ { "subtype", "set_max_thinking_tokens" } };
                    // 0 = thinking off; null = back to the START config, which is adaptive here
                    // (Claude Code's reset anchor for an explicit --thinking adaptive).
                    request["max_thinking_tokens"] = change.thinkingOff ? SdkJson(0) : SdkJson(nullptr);
                    const TapAnswer answer = tap->Request(std::move(request), kTapTimeout);
                    if (!answer.answered || !answer.ok)
                    {
                        // A Claude Code that refuses it once (a version without it, a policy) will
                        // refuse it again: from now on starts pass the literal --thinking value.
                        liveThinking.store(false);
                        LogHarness(options.name, std::wstring{ L"thinking: the live switch " } + (change.thinkingOff ? L"off" : L"on") + L" " + where + L" was " + (answer.answered ? L"REFUSED (" + HarnessFromUtf8(answer.error).substr(0, 200) + L")" : std::wstring{ L"NOT ANSWERED" }) + L"; a new process is started with the wanted --thinking value");
                        return false;
                    }
                    appliedThinking = change.thinkingOff ? HarnessThinking::Disabled : HarnessThinking::Adaptive;
                }
                // Toward "off": the body goes LAST, once Claude Code believes thinking is off (it
                // then also leaves out its clear_thinking strategy, which the API refuses next to
                // between_tools).
                if (change.betweenToolsOn && !SendRequestBody(true, where))
                {
                    return false;
                }
                if (!change.effort.empty())
                {
                    const TapAnswer answer = tap->Request(SdkJson{ { "subtype", "apply_flag_settings" }, { "settings", SdkJson{ { "effortLevel", HarnessToUtf8(change.effort) } } } }, kTapTimeout);
                    if (!answer.answered || !answer.ok)
                    {
                        liveEffort = false; // as above: later effort changes restart the process
                        LogHarness(options.name, L"thinking: the live effort change to " + change.effort + L" " + where + L" was " + (answer.answered ? L"REFUSED (" + HarnessFromUtf8(answer.error).substr(0, 200) + L")" : std::wstring{ L"NOT ANSWERED" }) + L"; a new process is started with --effort " + change.effort);
                        return false;
                    }
                    appliedEffort = change.effort;
                }
                const std::wstring inEffect = ReadEffortInEffect();
                {
                    std::lock_guard lock{ mutex };
                    inventory.thinking = ThinkingWord(appliedThinking, appliedBetweenTools);
                    inventory.effort = inEffect;
                }
                std::wstring line = std::wstring{ L"thinking " } + ThinkingWord(appliedThinking, appliedBetweenTools) + L", effort " + (inEffect.empty() ? std::wstring{ L"(not reported)" } : inEffect) + L" in effect";
                if (!appliedEffort.empty() && !inEffect.empty() && inEffect != appliedEffort)
                {
                    // Claude Code steps a level the model does not support down (max -> high), and
                    // a settings cap clamps it: what runs is what it reports.
                    line += L" (asked for " + appliedEffort + L"; Claude Code lowered it)";
                }
                line += change.Empty() ? L" (" + where + L")" : L" (changed live " + where + L")";
                LogHarness(options.name, line);
                return true;
            }
            catch (...)
            {
                LogSwallowedException(L"HarnessConversation::ApplyThinkingLive");
                return false;
            }
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
                return ReportStartFailure(std::move(failure), failureOut);
            }
            if (asked != firstAsked)
            {
                // A lower mode started where the higher one was refused: later starts of this
                // conversation ask for it directly, instead of failing once first.
                launchPermission.store(asked);
                LogHarness(options.name, std::wstring{ L"permission mode " } + HarnessPermissionModeWord(firstAsked) + L" is taken as NOT ALLOWED for this conversation; its later starts ask for " + HarnessPermissionModeWord(asked));
            }
            OnProcessUp(asked, started);
            // The thinking policy, before Ready (no message may reach a process that thinks when
            // it should not): a process wanted without thinking was started adaptive, so that it
            // can think again later without a restart — switch it off now.
            if (!ApplyThinkingLive(L"at start"))
            {
                // Claude Code did not take the switch (liveThinking is false now). One more start,
                // in the permission mode that just worked: its command line carries the literal
                // --thinking value, so nothing has to be switched live.
                Disconnect(true);
                std::wstring again = StartOnce(asked);
                if (!again.empty())
                {
                    return ReportStartFailure(std::move(again), failureOut);
                }
                OnProcessUp(asked, started);
                ApplyThinkingLive(L"at start"); // nothing to switch now; it reads and logs the reasoning in effect
            }
            SetState(HarnessState::Ready);
            return true;
        }

        // A start that failed for good: log it, forget the old process's report, and report the
        // failure (state Failed, an Error event). Worker thread only.
        //   failure    — why (the text of the Error event).
        //   failureOut — receives `failure`, for a message that waited for this process.
        //   returns    — false, always: EnsureProcess returns it.
        // Never throws.
        bool ReportStartFailure(std::wstring failure, std::wstring& failureOut) noexcept
        {
            LogHarness(options.name, L"start FAILED: " + failure);
            {
                std::lock_guard lock{ mutex };
                inventory = HarnessInventory{}; // no process answered: nothing is known
            }
            SetState(HarnessState::Failed, HarnessEventKind::Error, failure, true);
            failureOut = std::move(failure); // noexcept move: the caller reports it for a waiting message
            return false;
        }

        // The steps of every new process, between its `initialize` answer and Ready: fresh
        // per-process flags, a new inventory, the "process up" line, and the permission policy.
        // Worker thread only; `client` is connected.
        //   asked   — the permission mode the start asked for.
        //   started — when EnsureProcess began (the "in N ms" of the log line).
        // Never throws.
        void OnProcessUp(HarnessPermissionMode asked, std::chrono::steady_clock::time_point started) noexcept
        {
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
            HarnessModelReasoning reasoning;
            try
            {
                if (const auto info = client->get_server_info(); info && info->is_object())
                {
                    // ⚠ Only these two keys are read. The answer also holds the user's ACCOUNT
                    // object, which must never reach a log or the record.
                    if (const auto mode = info->find("current_permission_mode"); mode != info->end() && mode->is_string())
                    {
                        reported = HarnessFromUtf8(mode->get<std::string>());
                    }
                    // What the model can do in reasoning (its effort levels, adaptive thinking):
                    // the page's thinking toggle offers only the levels the model has.
                    if (const auto models = info->find("models"); models != info->end() && models->is_array())
                    {
                        reasoning = ReadHarnessModelReasoning(SdkJsonText(*models), options.model);
                    }
                }
            }
            catch (...)
            {
                LogSwallowedException(L"HarnessConversation::EnsureProcess (initialize answer)");
            }
            SettlePermissionMode(asked, reported, L"at start");
            // Whether "off" needs the API's between_tools is a fact of the RESOLVED model: --model
            // may be an alias ("sonnet"), and the alias moves with Claude Code's versions. The
            // at-start ApplyThinkingLive that follows reads it. (An unknown model keeps the
            // decision made from options.model.)
            if (reasoning.known && !reasoning.resolvedModel.empty())
            {
                modelRefusesDisabledThinking.store(HarnessModelRefusesDisabledThinking(reasoning.resolvedModel));
            }
            try
            {
                std::lock_guard lock{ mutex };
                inventory.supportedEfforts = std::move(reasoning.supportedEfforts);
                inventory.adaptiveThinking = reasoning.adaptiveThinking;
                inventory.thinking = ThinkingWord(appliedThinking, appliedBetweenTools);
            }
            catch (...)
            {
                LogSwallowedException(L"HarnessConversation::OnProcessUp (reasoning)");
            }
        }

        // -------------------------------------------------------------------------------
        // a turn
        // -------------------------------------------------------------------------------

        // Report the end of a turn (a TurnCompleted event) and write the record.
        //   turn / turnEpoch — the turn and the epoch it started in (a stale turn leaves the state alone).
        //   text             — the turn's final text, or why it failed.
        //   isError          — the turn failed; `text` then joins the record as an error notice.
        //   interrupted      — Interrupt, a newer Send or a Reset stopped it.
        //   durationMs       — from Send to the end, as the harness measured it.
        //   next             — the state after the turn.
        //   thinkingBlocks   — how many thinking blocks the model wrote in the turn (0 when unknown).
        //   thinkingTokens   — Claude Code's estimate of the turn's thinking tokens (0 when none or unknown).
        // Never throws.
        void FinishTurn(uint64_t turn, uint64_t turnEpoch, std::wstring text, bool isError, bool interrupted, int64_t durationMs, HarnessState next, int thinkingBlocks = 0, int64_t thinkingTokens = 0) noexcept
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
            e.thinkingBlocks = thinkingBlocks;
            e.thinkingTokens = thinkingTokens;
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
            case HarnessSystemKind::ThinkingTokens:
                // The deltas, summed: estimated_tokens restarts with each model request of the
                // turn (a tool call makes another), the deltas add up across them.
                turnThinkingTokens += facts.thinkingTokensDelta;
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
                int thinkingBlocks = 0; // the turn's thinking blocks (HarnessEvent::thinkingBlocks)
                turnThinkingTokens = 0; // this turn's thinking estimate starts here (OnSystemMessage adds to it)
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
                            if (std::holds_alternative<claude::ThinkingBlock>(block))
                            {
                                // Counted, not recorded: thinking is not part of the answer, and the
                                // count is the one direct sign that the turn's reasoning is on.
                                ++thinkingBlocks;
                                continue;
                            }
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
                        LogHarness(options.name, L"turn " + std::to_wstring(job.turn) + (failed ? L" FAILED" : (interruptSent ? L" interrupted" : L" done")) + L": " + std::to_wstring(elapsedMs()) + L" ms, " + std::to_wstring(facts.numTurns) + L" request(s), " + std::to_wstring(thinkingBlocks) + L" thinking block(s), ~" + std::to_wstring(turnThinkingTokens) + L" thinking token(s)" + (failed ? L": " + text.substr(0, 300) : std::wstring{}));
                        FinishTurn(job.turn, turnEpoch, std::move(text), failed, interruptSent, elapsedMs(), HarnessState::Ready, thinkingBlocks, turnThinkingTokens);
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
                bool ready = EnsureProcess(startFailure);
                if (ready && job && !ApplyThinkingLive(L"before turn " + std::to_wstring(job->turn)))
                {
                    // SetThinking asked for a reasoning the running process cannot take live: a new
                    // process starts with it on its command line, and the conversation continues
                    // from its record (the digest), as after a park.
                    LogHarness(options.name, L"the reasoning cannot be changed live; starting a new process with it (the conversation continues from its record)");
                    Disconnect(true);
                    ready = EnsureProcess(startFailure);
                }
                if (!ready)
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
        // The start values of the reasoning; SetThinking replaces them later.
        s.wantThinking = s.options.thinking;
        s.wantEffort = s.options.effort;
        // Before any process reported its resolved id: decided from the configured name (an
        // alias counts as a current model, Claude Code's own default). OnProcessUp refines it.
        s.modelRefusesDisabledThinking.store(HarnessModelRefusesDisabledThinking(s.options.model));
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

    void HarnessConversation::SetThinking(HarnessThinking thinking, std::wstring effort) noexcept
    {
        try
        {
            std::lock_guard lock{ _impl->mutex };
            if (_impl->stopRequested)
            {
                return;
            }
            // Only recorded here: the worker applies it before the next message (ApplyThinkingLive),
            // and a start in between reads it for its command line. No wake-up: an idle process
            // has nothing to do until a message comes.
            _impl->wantThinking = thinking;
            if (IsHarnessEffortWord(effort))
            {
                _impl->wantEffort = std::move(effort);
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: the old reasoning stays (std::lock_guard threw: the mutex is broken).
            LogSwallowedException(L"HarnessConversation::SetThinking");
        }
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
        // The reasoning wanted now: what the next start puts on its command line.
        HarnessThinking thinking = HarnessThinking::Disabled;
        std::wstring effort;
        {
            std::lock_guard lock{ _impl->mutex };
            thinking = _impl->wantThinking;
            effort = _impl->wantEffort;
        }
        // A transport that is never connected: build_command only reads the options.
        const claude::internal::SubprocessTransport transport{ _impl->BuildSdkOptions("claude.exe", _impl->launchPermission.load(), thinking, effort) };
        std::vector<std::wstring> out;
        for (const auto& arg : transport.build_command())
        {
            out.push_back(HarnessFromUtf8(arg));
        }
        return out;
    }
}
