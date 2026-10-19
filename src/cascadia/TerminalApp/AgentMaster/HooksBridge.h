// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — HooksBridge: hosts the local named pipe that the per-session PowerShell
// forwarder posts hook records to (HOOKS.md transport). Each record is parsed (HookWire.h)
// and handed to a sink (wired to SessionRegistry::OnHookEvent). No network surface.
//
// A small pool of pipe-server instances (same name, PIPE_UNLIMITED_INSTANCES) handles
// concurrent hook invocations across many sessions; each instance runs a
// connect -> read -> dispatch -> disconnect loop on its own thread and is woken to exit
// by a shared stop event.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "HookEvents.h"

namespace Agentmaster
{
    using HookSink = std::function<void(const HookMessage&)>;

    class HooksBridge
    {
    public:
        HooksBridge(std::wstring pipeName, HookSink sink, unsigned instances = 4);
        ~HooksBridge();

        HooksBridge(const HooksBridge&) = delete;
        HooksBridge& operator=(const HooksBridge&) = delete;

        // Start the listener threads. Idempotent. Returns false if the pipe could not be
        // created at all (the bridge then runs degraded — no hook state will arrive).
        bool Start();
        // Stop listening and join all threads. Safe to call multiple times / from dtor.
        void Stop() noexcept;

        const std::wstring& PipeName() const noexcept { return _pipeName; }
        bool Running() const noexcept { return _running.load(); }

    private:
        void _Worker() noexcept;

        std::wstring _pipeName;
        HookSink _sink;
        unsigned _instances;
        std::atomic<bool> _running{ false };
        void* _stopEvent{ nullptr }; // HANDLE (auto-reset is fine; manual-reset used to wake all)
        std::vector<std::thread> _threads;
    };

    // ===== the ASK pipe: Claude Code's AskUserQuestion, answered from Telegram (INTERACTIVE_TOOLS.md §6, MESSAGING.md §21.30) =====
    //
    // Claude Code runs a PermissionRequest hook BESIDE an open AskUserQuestion dialog (measured on 2.1.291). Agentmaster's hook,
    // <profile>\agentmaster-ask.ps1 (ClaudeSpawn.cpp BuildAskHookScript), connects to `\\.\pipe\agentmaster.<pid>.ask`, writes ONE
    // request line and BLOCKS on one reply line: a decision line (the answer from the phone) that it prints for Claude Code, or "{}"
    // (no decision: the dialog stays with the PC). The PC dialog stays usable the whole time; the first answer wins. Claude Code
    // does NOT end the hook when the PC answers first, so the engine releases it ("{}") as soon as it sees the answer in the
    // transcript — and the helper also ends by itself when its claude exits.

    /// <summary>Where one ask request stands.</summary>
    enum class AskEntryState
    {
        /// <summary>The helper waits for a reply; nothing was answered from here yet.</summary>
        Open,
        /// <summary>A decision is queued; the bridge writes it to the helper next.</summary>
        Answered,
        /// <summary>The decision went to the helper (it prints it; Claude Code takes it unless the PC answered first).</summary>
        Delivered,
        /// <summary>"{}" went to the helper: no decision, the dialog stays with the PC (answered there, or nothing could show it).</summary>
        Released,
        /// <summary>The helper went away before a decision: cancelled or answered at the PC and the hook ended, claude exited, the hook's timeout, or the app stopping.</summary>
        Gone,
    };

    /// <summary>One request as AskRegistry::Snapshot shows it.</summary>
    struct AskEntryInfo
    {
        uint64_t id{ 0 }; // the registry's id (never 0)
        std::wstring sessionId; // the hook payload's session_id
        std::wstring requestLine; // the helper's request line, decoded (AskQuestion.h ParseAskRequestLine reads it)
        int64_t openedMs{ 0 }; // when the request arrived (Unix ms)
        AskEntryState state{ AskEntryState::Open };
        int64_t changedMs{ 0 }; // when `state` last changed
        std::wstring why; // Released / Gone: why
    };

    /// <summary>
    /// The open AskUserQuestion requests of this process: the ask pipe's workers (AskBridge) open them, and the messaging service
    /// shows them, answers them or releases them. Thread-safe: every method takes the registry's own lock, which is never held
    /// across a call out except the wake-up's event signal. An ACCEPTOR decides at the door whether anything can answer a request
    /// (the Telegram bot is running); without one, every request is refused, and its helper hears "{}" at once.
    /// </summary>
    class AskRegistry
    {
    public:
        /// <summary>Decides whether a request of this session can be shown and answered (called on an ask worker thread).</summary>
        using Acceptor = std::function<bool(const std::wstring& sessionId)>;

        /// <summary>Sets (or, with an empty function, removes) the acceptor. The messaging service sets it while its bot runs.</summary>
        /// <param name="accept">The acceptor.</param>
        void SetAcceptor(Acceptor accept);
        /// <summary>
        /// Sets the function called after every change (a request opened, answered, written, released or gone) — the service's
        /// tick wake-up. Called with no registry lock held; it must not block.
        /// </summary>
        /// <param name="listener">The listener (empty = none).</param>
        void SetListener(std::function<void()> listener);

        /// <summary>
        /// The ask worker's door: records a request when the acceptor takes it. `wakeEvent` (a Win32 manual-reset event the worker
        /// owns) is signaled when a reply is queued for it; the registry forgets the handle at NoteWritten / NoteGone, before the
        /// worker closes it.
        /// </summary>
        /// <param name="sessionId">The payload's session_id.</param>
        /// <param name="requestLine">The decoded request line.</param>
        /// <param name="wakeEvent">The worker's event (a HANDLE).</param>
        /// <returns>The request's id, or 0 when refused (no acceptor, or it said no): the worker replies "{}".</returns>
        /// <exception cref="std::bad_alloc">If the entry cannot be stored (the worker contains it and replies "{}").</exception>
        uint64_t Open(const std::wstring& sessionId, std::wstring requestLine, void* wakeEvent);
        /// <summary>The reply queued for a request (a decision line, or "{}" for a release), taken once.</summary>
        /// <param name="id">The request.</param>
        /// <returns>The line, or nullopt when nothing is queued.</returns>
        /// <exception cref="std::bad_alloc">If the line cannot be copied.</exception>
        std::optional<std::string> TakeReply(uint64_t id);
        /// <summary>The worker wrote the queued reply: a decision becomes Delivered, a "{}" Released (and a failed write Gone).</summary>
        /// <param name="id">The request.</param>
        /// <param name="ok">The write completed.</param>
        void NoteWritten(uint64_t id, bool ok);
        /// <summary>The helper went away (or the bridge stops) before a reply was written: the request becomes Gone.</summary>
        /// <param name="id">The request.</param>
        /// <param name="why">Why, for the card and the log.</param>
        void NoteGone(uint64_t id, std::wstring why);

        /// <summary>Every request the service has not forgotten yet (open ones and finished ones), oldest first.</summary>
        /// <returns>The copies.</returns>
        /// <exception cref="std::bad_alloc">If the copies cannot be allocated.</exception>
        std::vector<AskEntryInfo> Snapshot() const;
        /// <summary>Queues the DECISION line for an open request and wakes its worker.</summary>
        /// <param name="id">The request.</param>
        /// <param name="decisionLine">The line (AskQuestion.h BuildAskAllowLine / BuildAskDenyLine), without its '\n'.</param>
        /// <returns>True when the request was open; false when it was answered, released or gone already (the PC was first).</returns>
        bool Answer(uint64_t id, std::string decisionLine);
        /// <summary>Queues "{}" for an open request (no decision: the dialog stays with the PC) and wakes its worker.</summary>
        /// <param name="id">The request.</param>
        /// <param name="why">Why, for the card and the log.</param>
        /// <returns>True when the request was open.</returns>
        bool Release(uint64_t id, std::wstring why);
        /// <summary>Drops a finished request (the service is done with it). An open one is left alone.</summary>
        /// <param name="id">The request.</param>
        void Forget(uint64_t id);

    private:
        struct Entry
        {
            AskEntryInfo info;
            void* wake{ nullptr }; // the worker's event while it waits; null once it has written or gone
            std::optional<std::string> reply; // the queued reply line
            bool replyIsDecision{ false }; // the queued reply is a decision (not "{}")
        };
        void _changed() const;

        mutable std::mutex _mtx;
        std::map<uint64_t, Entry> _entries;
        uint64_t _next{ 1 };
        Acceptor _accept;
        std::function<void()> _listener;
    };

    /// <summary>
    /// The ask pipe's server: a listener thread accepts each hook helper's connection and hands it to a worker thread of its own,
    /// which reads the request line (bounded: AskQuestion.h kAskRequestMaxBytes and 10 s), opens it in the registry, then waits
    /// for the reply the registry queues, for the helper to go away, or for Stop — and writes the reply. A refused request is
    /// answered "{}" at once. Raw Win32, no network surface; the pipe has the default security of the hook pipe (HooksBridge).
    /// </summary>
    class AskBridge
    {
    public:
        /// <summary>Creates the bridge stopped.</summary>
        /// <param name="pipeName">The pipe ("\\.\pipe\agentmaster.&lt;pid&gt;.ask").</param>
        /// <param name="registry">The registry the requests go into.</param>
        AskBridge(std::wstring pipeName, std::shared_ptr<AskRegistry> registry);
        /// <summary>Stop(). Never throws.</summary>
        ~AskBridge();
        AskBridge(const AskBridge&) = delete;
        AskBridge& operator=(const AskBridge&) = delete;

        /// <summary>Starts the listener. Idempotent.</summary>
        /// <returns>False when the stop event or the thread could not be made (the bridge then answers nothing; helpers find no pipe and leave the dialog to the PC).</returns>
        bool Start();
        /// <summary>
        /// Stops: every waiting helper hears "{}" (its dialog stays with the PC), every thread is joined. Idempotent; never throws.
        /// </summary>
        void Stop() noexcept;
        /// <summary>The pipe's name.</summary>
        const std::wstring& PipeName() const noexcept { return _pipeName; }

    private:
        void _Listen() noexcept;
        void _Serve(void* pipe) noexcept;
        void _ReapWorkers();

        struct Worker
        {
            std::thread thread;
            std::shared_ptr<std::atomic<bool>> done;
        };

        std::wstring _pipeName;
        std::shared_ptr<AskRegistry> _registry;
        std::atomic<bool> _running{ false };
        void* _stopEvent{ nullptr }; // manual-reset: wakes the listener and every worker
        std::thread _listener;
        std::mutex _workersMtx;
        std::vector<Worker> _workers;
    };
}
