// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
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
#include <functional>
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
}
