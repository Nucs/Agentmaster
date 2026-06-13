// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Agentmaster — the commandline introspection tool (doc/agentmaster/CLI.md).
//
// P1: read-only introspection. A console-subsystem binary that links ONLY the pure-C++ engine
// units (no WinRT, no TerminalAppLib) and reconstructs "what's going on" from state that ALWAYS
// exists — the persisted profile (sessions.json / windows/<id>.json), the live process table
// (ProcessInspect PEB reads), the Claude transcripts (TranscriptStore / ProcessInspect), and
// claude's own presence heartbeat (~/.claude/sessions/<pid>.json). It depends on no live
// responder, so it works whether or not the app is running — the Fleet Observer's pull model
// (OBSERVER.md), run one-shot from a separate process.
//
// Verbs: show <ref> | list | sessions | tabs | windows | external   (+ --self / --json).
// Control (restore/archive) is P2 — it rides the WM_COPYDATA handoff, not this reader.
//
// Build: standalone via cli/_compile.bat (the tests/ harness pattern). Packaged: a console
// vcxproj added to CascadiaPackage, invoked by the alias shim's verb dispatch (CLI.md §2/§8).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <appmodel.h>
#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../Activity.h"
#include "../ClaudeSpawn.h"
#include "../Json.h"
#include "../Persistence.h"
#include "../ProcessInspect.h"
#include "../SessionModels.h"
#include "../SessionScanner.h"
#include "../TranscriptStore.h"

using namespace Agentmaster;
namespace json = Agentmaster::json;

namespace
{
    constexpr int kSchemaVersion = 1;
    constexpr size_t kTailBytes = 131072; // transcript tail read for state + last reply (~128 KiB)

    // ===== output (UTF-8 to stdout) =========================================================

    std::string ToUtf8(std::wstring_view w)
    {
        if (w.empty())
        {
            return {};
        }
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<size_t>(n), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
        return s;
    }

    std::wstring FromUtf8(const char* p, size_t n)
    {
        if (!n)
        {
            return {};
        }
        const int m = ::MultiByteToWideChar(CP_UTF8, 0, p, static_cast<int>(n), nullptr, 0);
        std::wstring w(static_cast<size_t>(m), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, p, static_cast<int>(n), w.data(), m);
        return w;
    }

    void Out(std::wstring_view w)
    {
        const auto s = ToUtf8(w);
        ::fwrite(s.data(), 1, s.size(), stdout);
    }
    void OutLn(std::wstring_view w)
    {
        Out(w);
        ::fputc('\n', stdout);
    }
    void Err(std::wstring_view w)
    {
        const auto s = ToUtf8(w);
        ::fwrite(s.data(), 1, s.size(), stderr);
        ::fputc('\n', stderr);
    }

    // ===== time =============================================================================

    int64_t NowUnixMs()
    {
        FILETIME ft{};
        ::GetSystemTimeAsFileTime(&ft);
        ULARGE_INTEGER u{};
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        return static_cast<int64_t>(u.QuadPart / 10000ULL) - 11644473600000LL;
    }

    std::wstring FormatAgo(int64_t thenMs, int64_t nowMs)
    {
        if (thenMs <= 0)
        {
            return L"-";
        }
        int64_t d = nowMs - thenMs;
        if (d < 0)
        {
            d = 0;
        }
        const int64_t s = d / 1000;
        if (s < 5)
        {
            return L"just now";
        }
        if (s < 60)
        {
            return std::to_wstring(s) + L"s";
        }
        const int64_t m = s / 60;
        if (m < 60)
        {
            return std::to_wstring(m) + L"m";
        }
        const int64_t h = m / 60;
        if (h < 24)
        {
            return std::to_wstring(h) + L"h" + (m % 60 ? std::to_wstring(m % 60) + L"m" : L"");
        }
        const int64_t days = h / 24;
        if (days < 30)
        {
            return std::to_wstring(days) + L"d" + (h % 24 ? std::to_wstring(h % 24) + L"h" : L"");
        }
        const int64_t mo = days / 30;
        if (mo < 12)
        {
            return std::to_wstring(mo) + L"mo";
        }
        return std::to_wstring(days / 365) + L"y";
    }

    // ===== misc string helpers ==============================================================

    std::wstring Trunc(std::wstring_view s, size_t n)
    {
        // collapse newlines to a glyph for one-line table cells
        std::wstring out;
        out.reserve(std::min(s.size(), n));
        for (size_t i = 0; i < s.size() && out.size() < n; ++i)
        {
            const wchar_t c = s[i];
            out += (c == L'\n' || c == L'\r' || c == L'\t') ? L' ' : c;
        }
        if (s.size() > n)
        {
            out += L"…";
        }
        return out;
    }

    bool IEquals(std::wstring_view a, std::wstring_view b)
    {
        return a.size() == b.size() &&
               ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
    }

    bool IContains(std::wstring_view hay, std::wstring_view needle)
    {
        if (needle.empty())
        {
            return true;
        }
        if (needle.size() > hay.size())
        {
            return false;
        }
        for (size_t i = 0; i + needle.size() <= hay.size(); ++i)
        {
            if (::CompareStringOrdinal(hay.data() + i, static_cast<int>(needle.size()), needle.data(), static_cast<int>(needle.size()), TRUE) == CSTR_EQUAL)
            {
                return true;
            }
        }
        return false;
    }

    std::wstring EnvVar(const wchar_t* name)
    {
        const DWORD need = ::GetEnvironmentVariableW(name, nullptr, 0);
        if (need <= 1)
        {
            return {};
        }
        std::wstring v(need, L'\0');
        const DWORD got = ::GetEnvironmentVariableW(name, v.data(), need);
        v.resize(got);
        return v;
    }

    // Is this process running with MSIX package identity? When packaged, the profile resolves
    // automatically from GetCurrentPackageFamilyName (the dev exe -> dev profile, release ->
    // release), so the build-time brand default must NOT override it.
    bool IsPackaged()
    {
        UINT32 len = 0;
        return ::GetCurrentPackageFamilyName(&len, nullptr) != APPMODEL_ERROR_NO_PACKAGE;
    }

    // The owning-instance GUID prefix of an AM_SESSION stamp ("<guid>" or "<guid>:<windowId>").
    std::wstring AmGuidPrefix(std::wstring_view am)
    {
        const auto colon = am.find(L':');
        return std::wstring{ colon == std::wstring_view::npos ? am : am.substr(0, colon) };
    }

    // Read up to maxBytes from the END of a file, decode UTF-8, and (if we started mid-file)
    // drop the partial leading line so ParseTranscriptDelta sees whole JSONL lines. Shares
    // read/write/delete so a live claude is never blocked.
    std::wstring ReadFileTail(const std::wstring& path, size_t maxBytes)
    {
        const HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            return {};
        }
        LARGE_INTEGER sz{};
        if (!::GetFileSizeEx(h, &sz))
        {
            ::CloseHandle(h);
            return {};
        }
        const int64_t start = sz.QuadPart > static_cast<int64_t>(maxBytes) ? sz.QuadPart - static_cast<int64_t>(maxBytes) : 0;
        LARGE_INTEGER mv{};
        mv.QuadPart = start;
        ::SetFilePointerEx(h, mv, nullptr, FILE_BEGIN);
        std::string buf;
        buf.resize(static_cast<size_t>(sz.QuadPart - start));
        DWORD got = 0;
        const BOOL ok = ::ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr);
        ::CloseHandle(h);
        if (!ok)
        {
            return {};
        }
        buf.resize(got);
        size_t off = 0;
        if (start > 0)
        {
            const auto nl = buf.find('\n');
            if (nl != std::string::npos)
            {
                off = nl + 1;
            }
        }
        return FromUtf8(buf.data() + off, buf.size() - off);
    }

    // ===== the fleet snapshot ===============================================================

    struct LiveClaude
    {
        uint32_t pid{};
        uint32_t ppid{};
        ClaudeProcessFacts facts;
        std::wstring sessionId; // resolved (EMPTY => never-prompted / starting)
        int64_t createdMs{};
        int64_t lastActivityMs{};
        std::wstring presence; // claude's own heartbeat status (busy/idle/waiting/shell)
    };

    struct TabPlacement
    {
        int windowIdx{ -1 };
        int tabIdx{ -1 };
        bool selected{ false };
        std::wstring color;
    };

    struct Fleet
    {
        int64_t nowMs{};
        std::vector<SessionInfo> sessions; // managed (sessions.json)
        std::vector<WindowRecord> windows; // windows/*.json
        std::vector<SessionPresenceRow> presence;
        std::vector<LiveClaude> live; // process survey
        std::unordered_map<std::wstring, size_t> sessionById; // id -> index into sessions
        std::unordered_map<std::wstring, size_t> liveById; // id -> index into live
        std::unordered_map<std::wstring, TabPlacement> placeById; // id -> window/tab placement
    };

    // Bind a live claude to its CURRENT conversation id, MOST authoritative first:
    //   1. claude's OWN presence self-report (sessions/<pid>.json), matched by PID — immune to the
    //      cwd-density ambiguity that mis-binds when several claudes share one working dir (Rule #14);
    //   2. an explicit --session-id on the command line (known before the transcript exists);
    //   3. --resume <guid>;
    //   4. the cwd→transcript resolution by creation-time identity (the fragile fallback).
    // Empty => never-prompted / not yet resolvable.
    std::wstring ResolveLiveId(const ClaudeProcessFacts& fx, std::wstring_view presenceId)
    {
        if (!presenceId.empty() && IsSessionIdStem(presenceId))
        {
            return std::wstring{ presenceId };
        }
        if (!fx.sessionIdArg.empty() && IsSessionIdStem(fx.sessionIdArg))
        {
            return fx.sessionIdArg;
        }
        if (!fx.resumeTarget.empty() && IsSessionIdStem(fx.resumeTarget))
        {
            return fx.resumeTarget;
        }
        return ResolveSessionId(fx.cwd, fx.startUnixMs);
    }

    Fleet GatherFleet()
    {
        Fleet f;
        f.nowMs = NowUnixMs();
        f.sessions = LoadSessions();
        f.windows = LoadWindowRecords();
        f.presence = ReadSessionPresence();

        for (size_t i = 0; i < f.sessions.size(); ++i)
        {
            f.sessionById[f.sessions[i].id] = i;
        }

        std::unordered_map<uint32_t, const SessionPresenceRow*> presByPid;
        for (const auto& p : f.presence)
        {
            presByPid[p.pid] = &p; // f.presence is stable after load (not reallocated below)
        }

        const auto snap = SnapshotProcesses();
        // Set of claude pids, so we can drop Task-tool SUBAGENTS (a claude.exe whose PARENT is also
        // claude.exe): those are ephemeral helpers, not user sessions — a real session's parent is a
        // shell/ConPTY, never claude. Without this they'd inflate the census and a subagent could
        // resolve onto its parent's transcript and falsely mark a session live.
        std::unordered_set<uint32_t> claudePids;
        for (const auto& e : snap)
        {
            if (ImageNameEq(e.image, L"claude.exe"))
            {
                claudePids.insert(e.pid);
            }
        }
        for (const auto& e : snap)
        {
            if (!ImageNameEq(e.image, L"claude.exe"))
            {
                continue;
            }
            if (claudePids.count(e.ppid) > 0)
            {
                continue; // subagent (parent is claude)
            }
            LiveClaude lc;
            lc.pid = e.pid;
            lc.ppid = e.ppid;
            lc.facts = ReadClaudeFacts(e.pid);
            lc.facts.pid = e.pid;
            lc.facts.parentPid = e.ppid;
            const SessionPresenceRow* pres = nullptr;
            if (const auto it = presByPid.find(e.pid); it != presByPid.end())
            {
                pres = it->second;
                lc.presence = pres->status;
            }
            lc.sessionId = ResolveLiveId(lc.facts, pres ? pres->sessionId : std::wstring_view{});
            if (!lc.sessionId.empty())
            {
                TranscriptTimes(lc.facts.cwd, lc.sessionId, lc.createdMs, lc.lastActivityMs);
            }
            f.live.push_back(std::move(lc));
        }
        for (size_t i = 0; i < f.live.size(); ++i)
        {
            if (!f.live[i].sessionId.empty())
            {
                f.liveById[f.live[i].sessionId] = i;
            }
        }

        for (int wi = 0; wi < static_cast<int>(f.windows.size()); ++wi)
        {
            const auto& w = f.windows[wi];
            for (int ti = 0; ti < static_cast<int>(w.tabs.size()); ++ti)
            {
                const auto& t = w.tabs[ti];
                if (t.kind == TabKind::Claude && !t.sessionId.empty())
                {
                    TabPlacement p;
                    p.windowIdx = wi;
                    p.tabIdx = ti;
                    p.color = t.tabColor;
                    p.selected = IEquals(w.selectedSessionId, t.sessionId);
                    f.placeById[t.sessionId] = p;
                }
            }
        }
        return f;
    }

    // ===== derived state (offline) ==========================================================

    struct DerivedState
    {
        std::wstring state; // Running / WaitingForInput / NeedsApproval / Idle / Archived
        std::wstring presence; // raw heartbeat
        bool turnInFlight{ false };
        std::wstring lastAssistant; // where the conversation left off
        std::wstring lastStopReason;
        std::wstring pendingTool; // an unanswered interactive tool (AskUserQuestion) => NeedsApproval
        std::vector<std::wstring> lastTurnTools;
    };

    struct Turn
    {
        std::wstring role; // "user" | "assistant"
        std::wstring text;
        std::wstring stop; // assistant only
        std::wstring tool; // assistant only (interactive tool name)
    };

    // Parse the transcript tail once: derive state (presence-primary, transcript-refined) and the
    // ordered recent turns. `transcriptPath` empty / unreadable => only presence/alive drive state.
    DerivedState DeriveState(const std::wstring& transcriptPath, std::wstring_view presence, bool alive, std::vector<Turn>* outTurns, size_t tailBytes = kTailBytes)
    {
        DerivedState d;
        d.presence = presence;

        std::wstring lastStop;
        std::wstring pendingTool;
        bool interrupted = false;

        if (!transcriptPath.empty())
        {
            const auto chunk = ReadFileTail(transcriptPath, tailBytes);
            if (!chunk.empty())
            {
                const auto parsed = ParseTranscriptDelta(chunk);
                for (const auto& ev : parsed.events)
                {
                    if (ev.kind == TranscriptEvent::Kind::Assistant)
                    {
                        if (!ev.text.empty())
                        {
                            d.lastAssistant = ev.text;
                        }
                        lastStop = ev.stopReason;
                        if (!ev.toolName.empty())
                        {
                            pendingTool = ev.toolName;
                            d.lastTurnTools.push_back(ev.toolName);
                        }
                        // Only record assistant turns that carry human-readable TEXT — a pure
                        // tool_use / thinking line has empty text and would just be blank noise in
                        // the conversation view.
                        if (outTurns && !ev.text.empty())
                        {
                            outTurns->push_back({ L"assistant", ev.text, ev.stopReason, ev.toolName });
                        }
                    }
                    else if (ev.kind == TranscriptEvent::Kind::UserPrompt)
                    {
                        if (IsUserInterruptMarker(ev.text))
                        {
                            interrupted = true;
                        }
                        else
                        {
                            // a fresh human turn starts: clear the prior turn's tail facts
                            interrupted = false;
                            lastStop.clear();
                            pendingTool.clear();
                            d.lastTurnTools.clear();
                            if (outTurns)
                            {
                                outTurns->push_back({ L"user", ev.text, L"", L"" });
                            }
                        }
                    }
                    else if (ev.kind == TranscriptEvent::Kind::ToolResult)
                    {
                        // a (non-interactive) tool completed
                        pendingTool.clear();
                    }
                }
            }
        }

        d.lastStopReason = lastStop;
        d.pendingTool = pendingTool;
        d.turnInFlight = !IsTerminalStopReason(lastStop) && !interrupted;

        if (!alive)
        {
            d.state = L"Archived";
            return d;
        }
        const bool blockedOnUser = !pendingTool.empty() && IsInteractiveTool(pendingTool);
        if (presence == L"busy")
        {
            d.state = L"Running";
        }
        else if (presence == L"waiting")
        {
            d.state = blockedOnUser ? L"NeedsApproval" : L"WaitingForInput";
        }
        else if (presence == L"idle" || presence == L"shell")
        {
            d.state = blockedOnUser ? L"NeedsApproval" : L"Idle";
        }
        else
        {
            // no presence file — derive purely from the transcript tail
            if (interrupted)
            {
                d.state = L"WaitingForInput";
            }
            else if (d.turnInFlight)
            {
                d.state = L"Running";
            }
            else if (blockedOnUser)
            {
                d.state = L"NeedsApproval";
            }
            else
            {
                d.state = L"WaitingForInput";
            }
        }
        return d;
    }

    // Resolve a managed session's transcript path. Prefer the global id glob (robust to a moved
    // cwd); fall back to the live claude's cwd encoding.
    std::wstring TranscriptPathFor(const std::wstring& id, std::wstring_view cwdHint)
    {
        auto p = ResolveClaudeTranscriptPath(id);
        if (!p.empty())
        {
            return p;
        }
        if (!cwdHint.empty())
        {
            // ResolveClaudeTranscriptPath globs all project dirs; if it missed, nothing else will.
        }
        return p;
    }

    // ===== JSON builders ====================================================================

    json::Value JTiming(int64_t createdMs, int64_t lastMs, int64_t nowMs)
    {
        auto v = json::Value::MkObj();
        v.Set(L"createdUnixMs", json::Value::MkNum(static_cast<double>(createdMs)));
        v.Set(L"lastActivityUnixMs", json::Value::MkNum(static_cast<double>(lastMs)));
        v.Set(L"createdAgo", json::Value::MkStr(FormatAgo(createdMs, nowMs)));
        v.Set(L"lastActivityAgo", json::Value::MkStr(FormatAgo(lastMs, nowMs)));
        return v;
    }

    json::Value JQueue(const std::vector<QueuedPrompt>& q)
    {
        auto arr = json::Value::MkArr();
        for (const auto& p : q)
        {
            auto o = json::Value::MkObj();
            o.Set(L"id", json::Value::MkStr(p.id));
            o.Set(L"label", json::Value::MkStr(p.label));
            o.Set(L"text", json::Value::MkStr(p.text));
            o.Set(L"status", json::Value::MkStr(ToString(p.status)));
            o.Set(L"origin", json::Value::MkStr(ToString(p.origin)));
            o.Set(L"gate", json::Value::MkStr(ToString(p.gate)));
            if (p.sentAtUnixMs)
            {
                o.Set(L"sentAtUnixMs", json::Value::MkNum(static_cast<double>(p.sentAtUnixMs)));
            }
            arr.Push(std::move(o));
        }
        return arr;
    }

    json::Value JAutopilot(const AutopilotState& a)
    {
        auto o = json::Value::MkObj();
        o.Set(L"mode", json::Value::MkStr(ToString(a.mode)));
        o.Set(L"maxAutoSends", json::Value::MkNum(a.maxAutoSends));
        o.Set(L"autoSendsThisRun", json::Value::MkNum(a.autoSendsThisRun));
        o.Set(L"stopOnError", json::Value::MkBool(a.stopOnError));
        o.Set(L"pauseOnHumanInput", json::Value::MkBool(a.pauseOnHumanInput));
        return o;
    }

    json::Value JPlacement(const Fleet& f, const std::wstring& id)
    {
        const auto it = f.placeById.find(id);
        if (it == f.placeById.end())
        {
            return json::Value{};
        }
        const auto& p = it->second;
        auto o = json::Value::MkObj();
        if (p.windowIdx >= 0 && p.windowIdx < static_cast<int>(f.windows.size()))
        {
            o.Set(L"windowId", json::Value::MkStr(f.windows[p.windowIdx].windowId));
        }
        o.Set(L"windowIndex", json::Value::MkNum(p.windowIdx));
        o.Set(L"tabIndex", json::Value::MkNum(p.tabIdx));
        o.Set(L"selected", json::Value::MkBool(p.selected));
        if (!p.color.empty())
        {
            o.Set(L"color", json::Value::MkStr(p.color));
        }
        return o;
    }

    // The compact per-session object (for list / sessions). `full` adds conversation + queue body.
    json::Value JSession(const Fleet& f, const SessionInfo& s, bool full, int tailTurns)
    {
        const auto liveIt = f.liveById.find(s.id);
        const LiveClaude* lc = liveIt != f.liveById.end() ? &f.live[liveIt->second] : nullptr;
        const bool alive = lc != nullptr;

        const std::wstring cwd = lc && !lc->facts.cwd.empty() ? lc->facts.cwd : s.workingDir;
        const std::wstring tpath = TranscriptPathFor(s.id, cwd);

        std::vector<Turn> turns;
        // `show` reads a deeper tail (more turns of history); the list/sessions views only need
        // enough to derive state.
        const size_t tailBytes = full ? (kTailBytes * 4) : kTailBytes;
        const auto ds = DeriveState(tpath, lc ? lc->presence : L"", alive, full ? &turns : nullptr, tailBytes);

        int64_t createdMs = s.convCreatedUnixMs;
        int64_t lastMs = s.convLastActivityUnixMs;
        if (lc)
        {
            createdMs = lc->createdMs ? lc->createdMs : createdMs;
            lastMs = lc->lastActivityMs ? lc->lastActivityMs : lastMs;
        }
        if ((!createdMs || !lastMs) && !tpath.empty())
        {
            int64_t c = 0, l = 0;
            if (TranscriptTimes(cwd, s.id, c, l))
            {
                if (!createdMs)
                {
                    createdMs = c;
                }
                if (!lastMs)
                {
                    lastMs = l;
                }
            }
        }

        auto o = json::Value::MkObj();
        o.Set(L"id", json::Value::MkStr(s.id));
        o.Set(L"title", json::Value::MkStr(s.title));
        o.Set(L"workingDir", json::Value::MkStr(s.workingDir));
        if (lc && !lc->facts.cwd.empty() && !IEquals(lc->facts.cwd, s.workingDir))
        {
            o.Set(L"liveCwd", json::Value::MkStr(lc->facts.cwd));
        }
        o.Set(L"branch", json::Value::MkStr(s.branch));
        o.Set(L"managed", json::Value::MkBool(true));
        o.Set(L"external", json::Value::MkBool(s.external));
        o.Set(L"live", json::Value::MkBool(alive));
        o.Set(L"linkState", json::Value::MkStr(alive ? (s.external ? L"external" : L"linked") : L"archived"));
        o.Set(L"derivedState", json::Value::MkStr(ds.state));
        if (!ds.presence.empty())
        {
            o.Set(L"presence", json::Value::MkStr(ds.presence));
        }
        o.Set(L"turnInFlight", json::Value::MkBool(ds.turnInFlight));
        if (lc)
        {
            o.Set(L"pid", json::Value::MkNum(lc->pid));
            if (!lc->facts.model.empty())
            {
                o.Set(L"model", json::Value::MkStr(lc->facts.model));
            }
            if (!lc->facts.effort.empty())
            {
                o.Set(L"effort", json::Value::MkStr(lc->facts.effort));
            }
            if (!lc->facts.permissionMode.empty())
            {
                o.Set(L"permissionMode", json::Value::MkStr(lc->facts.permissionMode));
            }
        }
        o.Set(L"autopilot", json::Value::MkStr(ToString(s.autopilot.mode)));
        // queue counts
        {
            int pending = 0, sent = 0, held = 0;
            for (const auto& p : s.queue)
            {
                if (p.status == PromptStatus::Pending)
                {
                    ++pending;
                }
                else if (p.status == PromptStatus::Sent)
                {
                    ++sent;
                }
                else if (p.status == PromptStatus::Held)
                {
                    ++held;
                }
            }
            auto qc = json::Value::MkObj();
            qc.Set(L"total", json::Value::MkNum(static_cast<double>(s.queue.size())));
            qc.Set(L"pending", json::Value::MkNum(pending));
            qc.Set(L"sent", json::Value::MkNum(sent));
            qc.Set(L"held", json::Value::MkNum(held));
            o.Set(L"queueCounts", std::move(qc));
        }
        o.Set(L"timing", JTiming(createdMs, lastMs, f.nowMs));
        if (auto pl = JPlacement(f, s.id); pl.type == json::Value::Type::Obj)
        {
            o.Set(L"placement", std::move(pl));
        }

        if (full)
        {
            o.Set(L"autopilotDetail", JAutopilot(s.autopilot));
            o.Set(L"flightPlan", JQueue(s.queue));
            if (!ds.lastAssistant.empty())
            {
                o.Set(L"lastAssistantReply", json::Value::MkStr(ds.lastAssistant));
            }
            if (!ds.lastStopReason.empty())
            {
                o.Set(L"lastStopReason", json::Value::MkStr(ds.lastStopReason));
            }
            if (!ds.pendingTool.empty())
            {
                o.Set(L"pendingInteractiveTool", json::Value::MkStr(ds.pendingTool));
            }
            // conversation tail (last `tailTurns` turns)
            auto conv = json::Value::MkArr();
            const int start = tailTurns > 0 && static_cast<int>(turns.size()) > tailTurns ? static_cast<int>(turns.size()) - tailTurns : 0;
            for (int i = start; i < static_cast<int>(turns.size()); ++i)
            {
                auto t = json::Value::MkObj();
                t.Set(L"role", json::Value::MkStr(turns[i].role));
                t.Set(L"text", json::Value::MkStr(turns[i].text));
                if (!turns[i].stop.empty())
                {
                    t.Set(L"stopReason", json::Value::MkStr(turns[i].stop));
                }
                if (!turns[i].tool.empty())
                {
                    t.Set(L"tool", json::Value::MkStr(turns[i].tool));
                }
                conv.Push(std::move(t));
            }
            o.Set(L"conversation", std::move(conv));
            o.Set(L"transcriptPath", json::Value::MkStr(tpath));

            // The human's last real ask (what the session is working on / waiting to act on).
            for (auto it = turns.rbegin(); it != turns.rend(); ++it)
            {
                if (it->role == L"user" && !it->text.empty())
                {
                    o.Set(L"lastUserPrompt", json::Value::MkStr(it->text));
                    break;
                }
            }

            // Activity + what files it's touching — a full transcript fold (one session, so the
            // whole-file scan is fine). This is the "what is it actually doing" signal.
            if (!tpath.empty())
            {
                TranscriptStats st;
                if (AccumulateTranscriptStats(tpath, st))
                {
                    auto act = json::Value::MkObj();
                    act.Set(L"messages", json::Value::MkNum(st.userPrompts + st.assistantLines));
                    act.Set(L"userPrompts", json::Value::MkNum(st.userPrompts));
                    act.Set(L"assistantLines", json::Value::MkNum(st.assistantLines));
                    act.Set(L"toolUses", json::Value::MkNum(st.toolUses));
                    o.Set(L"activity", std::move(act));
                    if (!st.pathsAccessed.empty())
                    {
                        auto files = json::Value::MkArr();
                        for (size_t i = 0; i < st.pathsAccessed.size() && i < 40; ++i)
                        {
                            files.Push(json::Value::MkStr(st.pathsAccessed[i]));
                        }
                        o.Set(L"filesTouched", std::move(files));
                    }
                }
            }
        }
        return o;
    }

    // External (unmanaged) live claude row.
    json::Value JExternal(const Fleet& f, const LiveClaude& lc)
    {
        auto o = json::Value::MkObj();
        o.Set(L"pid", json::Value::MkNum(lc.pid));
        o.Set(L"sessionId", json::Value::MkStr(lc.sessionId));
        o.Set(L"cwd", json::Value::MkStr(lc.facts.cwd));
        if (!lc.facts.model.empty())
        {
            o.Set(L"model", json::Value::MkStr(lc.facts.model));
        }
        if (!lc.facts.effort.empty())
        {
            o.Set(L"effort", json::Value::MkStr(lc.facts.effort));
        }
        if (!lc.facts.wtSession.empty())
        {
            o.Set(L"wtSession", json::Value::MkStr(lc.facts.wtSession));
        }
        o.Set(L"managed", json::Value::MkBool(false));
        // Classify the host so a consumer can tell a TRULY-external claude (real Windows Terminal
        // / bare console) from one managed by the OTHER Agentmaster instance (release vs dev — both
        // install side by side). AM_SESSION is stamped by whichever Agentmaster launched it; its
        // GUID prefix identifies the instance.
        {
            const std::wstring ourAm = EnvVar(L"AM_SESSION");
            std::wstring host;
            if (!lc.facts.amSession.empty())
            {
                const bool self = !ourAm.empty() && IEquals(AmGuidPrefix(lc.facts.amSession), AmGuidPrefix(ourAm));
                host = self ? L"agentmaster-self" : L"agentmaster-other";
                o.Set(L"amSession", json::Value::MkStr(lc.facts.amSession));
            }
            else if (!lc.facts.wtSession.empty())
            {
                host = L"windows-terminal";
            }
            else
            {
                host = L"console";
            }
            o.Set(L"host", json::Value::MkStr(host));
        }
        // enrich from transcript (title/branch/prompts)
        std::wstring title;
        if (!lc.sessionId.empty() && !lc.facts.cwd.empty())
        {
            const auto info = ReadTranscriptInfo(lc.facts.cwd, lc.sessionId, 65536, 1);
            title = TranscriptDisplayTitle(info);
            if (!info.gitBranch.empty())
            {
                o.Set(L"branch", json::Value::MkStr(info.gitBranch));
            }
        }
        o.Set(L"title", json::Value::MkStr(title.empty() ? (lc.sessionId.empty() ? L"(starting — no prompt yet)" : L"claude") : title));
        const std::wstring tpath = lc.sessionId.empty() ? L"" : ResolveClaudeTranscriptPath(lc.sessionId);
        const auto ds = DeriveState(tpath, lc.presence, true, nullptr);
        o.Set(L"derivedState", json::Value::MkStr(ds.state));
        if (!ds.presence.empty())
        {
            o.Set(L"presence", json::Value::MkStr(ds.presence));
        }
        o.Set(L"timing", JTiming(lc.createdMs, lc.lastActivityMs, f.nowMs));
        return o;
    }

    // Find first matching string member at any depth (for cheap Other-tab labels).
    const std::wstring* FindMemberDeep(const json::Value& v, std::wstring_view key)
    {
        if (v.type == json::Value::Type::Obj)
        {
            for (const auto& [k, c] : v.members)
            {
                if (k == key && c.type == json::Value::Type::Str)
                {
                    return &c.str;
                }
            }
            for (const auto& [k, c] : v.members)
            {
                if (const auto* r = FindMemberDeep(c, key))
                {
                    return r;
                }
            }
        }
        else if (v.type == json::Value::Type::Arr)
        {
            for (const auto& c : v.arr)
            {
                if (const auto* r = FindMemberDeep(c, key))
                {
                    return r;
                }
            }
        }
        return nullptr;
    }

    std::wstring OtherTabLabel(const std::wstring& actionsJson)
    {
        if (actionsJson.empty())
        {
            return L"(shell)";
        }
        const auto parsed = json::Parse(actionsJson);
        if (!parsed)
        {
            return L"(shell)";
        }
        for (const auto* key : { L"tabTitle", L"commandline", L"startingDirectory" })
        {
            if (const auto* s = FindMemberDeep(*parsed, key); s && !s->empty())
            {
                return Trunc(*s, 60);
            }
        }
        return L"(shell)";
    }

    json::Value JTab(const Fleet& f, const WindowRecord& w, const TabEntry& t, int tabIdx)
    {
        auto o = json::Value::MkObj();
        o.Set(L"index", json::Value::MkNum(tabIdx));
        const bool isClaude = t.kind == TabKind::Claude;
        o.Set(L"kind", json::Value::MkStr(isClaude ? L"claude" : L"shell"));
        const bool selected = isClaude ? IEquals(w.selectedSessionId, t.sessionId) : (w.selectedTabIndex == tabIdx);
        o.Set(L"selected", json::Value::MkBool(selected));
        if (isClaude)
        {
            o.Set(L"sessionId", json::Value::MkStr(t.sessionId));
            const auto sit = f.sessionById.find(t.sessionId);
            if (sit != f.sessionById.end())
            {
                const auto& s = f.sessions[sit->second];
                o.Set(L"title", json::Value::MkStr(s.title));
                const bool alive = f.liveById.count(t.sessionId) > 0;
                o.Set(L"live", json::Value::MkBool(alive));
                o.Set(L"linkState", json::Value::MkStr(alive ? L"linked" : L"archived"));
            }
            else
            {
                o.Set(L"title", json::Value::MkStr(L"(unknown session)"));
            }
            if (!t.tabColor.empty())
            {
                o.Set(L"color", json::Value::MkStr(t.tabColor));
            }
        }
        else
        {
            o.Set(L"title", json::Value::MkStr(OtherTabLabel(t.actionsJson)));
        }
        return o;
    }

    json::Value JGeometry(const WindowGeometry& g)
    {
        auto o = json::Value::MkObj();
        if (g.hasPosition)
        {
            o.Set(L"x", json::Value::MkNum(g.x));
            o.Set(L"y", json::Value::MkNum(g.y));
        }
        if (g.hasSize)
        {
            o.Set(L"width", json::Value::MkNum(g.width));
            o.Set(L"height", json::Value::MkNum(g.height));
        }
        if (!g.launchMode.empty())
        {
            o.Set(L"launchMode", json::Value::MkStr(g.launchMode));
        }
        return o;
    }

    json::Value JWindow(const Fleet& f, const WindowRecord& w, bool withTabs, bool withLens)
    {
        auto o = json::Value::MkObj();
        o.Set(L"windowId", json::Value::MkStr(w.windowId));
        o.Set(L"geometry", JGeometry(w.geometry));
        o.Set(L"tabCount", json::Value::MkNum(static_cast<double>(w.tabs.size())));
        if (withLens)
        {
            auto m = json::Value::MkObj();
            m.Set(L"selectedId", json::Value::MkStr(w.manager.selectedId));
            m.Set(L"scopeDir", json::Value::MkStr(w.manager.scopeDir));
            m.Set(L"treeScope", json::Value::MkNum(w.manager.treeScope));
            o.Set(L"managerLens", std::move(m));
        }
        if (withTabs)
        {
            auto arr = json::Value::MkArr();
            for (int ti = 0; ti < static_cast<int>(w.tabs.size()); ++ti)
            {
                arr.Push(JTab(f, w, w.tabs[ti], ti));
            }
            o.Set(L"tabs", std::move(arr));
        }
        return o;
    }

    // ===== reference resolution (for show / future control) =================================

    // A LIVE claude matching a ref by exact id or id-prefix (for sessions not in THIS profile's
    // sessions.json — managed by the other instance, or not yet persisted).
    const LiveClaude* FindLiveByRef(const Fleet& f, std::wstring_view ref)
    {
        for (const auto& lc : f.live)
        {
            if (!lc.sessionId.empty() && (IEquals(lc.sessionId, ref) || lc.sessionId.rfind(ref, 0) == 0))
            {
                return &lc;
            }
        }
        return nullptr;
    }

    // Resolve a user-supplied ref to a managed session index, or -1 with candidates printed.
    int ResolveSessionRef(const Fleet& f, std::wstring_view ref, std::vector<std::wstring>& candidates)
    {
        // 1. exact id
        if (const auto it = f.sessionById.find(std::wstring{ ref }); it != f.sessionById.end())
        {
            return static_cast<int>(it->second);
        }
        // 2. id prefix / title substring
        std::vector<int> hits;
        for (size_t i = 0; i < f.sessions.size(); ++i)
        {
            const auto& s = f.sessions[i];
            if (s.id.rfind(ref, 0) == 0 || IContains(s.title, ref))
            {
                hits.push_back(static_cast<int>(i));
            }
        }
        if (hits.size() == 1)
        {
            return hits[0];
        }
        for (const int h : hits)
        {
            candidates.push_back(f.sessions[h].id + L"  " + f.sessions[h].title);
        }
        return -1;
    }

    // ===== commands =========================================================================

    struct Args
    {
        std::wstring verb;
        std::wstring ref;
        bool json{ false };
        bool self{ false };
        bool archived{ false };
        int tail{ 8 };
        std::wstring stateFilter;
        std::wstring dirFilter;
        std::wstring windowFilter;
    };

    int EmitJson(const json::Value& v)
    {
        OutLn(json::Dump(v));
        return 0;
    }

    json::Value EnvelopeBase()
    {
        auto root = json::Value::MkObj();
        root.Set(L"schemaVersion", json::Value::MkNum(kSchemaVersion));
        root.Set(L"profile", json::Value::MkStr(AgentmasterStateDir()));
        return root;
    }

    int CmdSessions(const Fleet& f, const Args& a)
    {
        std::vector<const SessionInfo*> rows;
        for (const auto& s : f.sessions)
        {
            const bool alive = f.liveById.count(s.id) > 0;
            if (!a.archived && !alive)
            {
                continue; // default: only OPEN (live) sessions, like the Board
            }
            if (!a.dirFilter.empty() && !IContains(s.workingDir, a.dirFilter))
            {
                continue;
            }
            rows.push_back(&s);
        }

        if (a.json)
        {
            auto root = EnvelopeBase();
            auto arr = json::Value::MkArr();
            for (const auto* s : rows)
            {
                auto js = JSession(f, *s, false, a.tail);
                if (!a.stateFilter.empty() && !IEquals(js.StrAt(L"derivedState"), a.stateFilter))
                {
                    continue;
                }
                arr.Push(std::move(js));
            }
            root.Set(L"sessions", std::move(arr));
            return EmitJson(root);
        }

        OutLn(L"STATE         LIVE  TITLE                          DIR                              QUEUE");
        for (const auto* s : rows)
        {
            const auto js = JSession(f, *s, false, a.tail);
            const auto st = js.StrAt(L"derivedState");
            if (!a.stateFilter.empty() && !IEquals(st, a.stateFilter))
            {
                continue;
            }
            const bool alive = js.BoolAt(L"live");
            const auto qc = js.Find(L"queueCounts");
            const int pend = qc ? static_cast<int>(qc->U32At(L"pending")) : 0;
            const int tot = qc ? static_cast<int>(qc->U32At(L"total")) : 0;
            std::wstring line;
            line += st;
            line.resize(std::max<size_t>(line.size(), 14), L' ');
            line += alive ? L" ●    " : L" ○    ";
            line += Trunc(s->title, 30);
            line.resize(std::max<size_t>(line.size(), 14 + 6 + 31), L' ');
            line += Trunc(s->workingDir, 32);
            line.resize(std::max<size_t>(line.size(), 14 + 6 + 31 + 33), L' ');
            line += std::to_wstring(pend) + L"/" + std::to_wstring(tot);
            OutLn(line);
        }
        return 0;
    }

    int CmdExternal(const Fleet& f, const Args& a)
    {
        std::vector<const LiveClaude*> ext;
        for (const auto& lc : f.live)
        {
            const bool managed = !lc.sessionId.empty() && f.sessionById.count(lc.sessionId) > 0;
            if (!managed)
            {
                ext.push_back(&lc);
            }
        }
        if (a.json)
        {
            auto root = EnvelopeBase();
            auto arr = json::Value::MkArr();
            for (const auto* lc : ext)
            {
                arr.Push(JExternal(f, *lc));
            }
            root.Set(L"external", std::move(arr));
            return EmitJson(root);
        }
        OutLn(L"PID     HOST              STATE            CWD                                  TITLE");
        for (const auto* lc : ext)
        {
            const auto j = JExternal(f, *lc);
            std::wstring line = std::to_wstring(lc->pid);
            line.resize(std::max<size_t>(line.size(), 8), L' ');
            line += j.StrAt(L"host");
            line.resize(std::max<size_t>(line.size(), 8 + 18), L' ');
            line += j.StrAt(L"derivedState");
            line.resize(std::max<size_t>(line.size(), 8 + 18 + 17), L' ');
            line += Trunc(lc->facts.cwd, 36);
            line.resize(std::max<size_t>(line.size(), 8 + 18 + 17 + 37), L' ');
            line += Trunc(j.StrAt(L"title"), 40);
            OutLn(line);
        }
        if (ext.empty())
        {
            OutLn(L"(no external claudes)");
        }
        return 0;
    }

    int CmdWindows(const Fleet& f, const Args& a)
    {
        if (a.json)
        {
            auto root = EnvelopeBase();
            auto arr = json::Value::MkArr();
            for (const auto& w : f.windows)
            {
                arr.Push(JWindow(f, w, true, true));
            }
            root.Set(L"windows", std::move(arr));
            return EmitJson(root);
        }
        for (const auto& w : f.windows)
        {
            std::wstring head = L"WINDOW " + Trunc(w.windowId, 38) + L"  tabs=" + std::to_wstring(w.tabs.size());
            if (w.geometry.hasSize)
            {
                head += L"  " + std::to_wstring(static_cast<int>(w.geometry.width)) + L"x" + std::to_wstring(static_cast<int>(w.geometry.height));
            }
            OutLn(head);
            for (int ti = 0; ti < static_cast<int>(w.tabs.size()); ++ti)
            {
                const auto j = JTab(f, w, w.tabs[ti], ti);
                std::wstring line = L"  [" + std::to_wstring(ti) + L"] " + j.StrAt(L"kind") + L"  " + Trunc(j.StrAt(L"title"), 50);
                if (j.BoolAt(L"selected"))
                {
                    line += L"  *";
                }
                OutLn(line);
            }
        }
        if (f.windows.empty())
        {
            OutLn(L"(no saved windows)");
        }
        return 0;
    }

    int CmdTabs(const Fleet& f, const Args& a)
    {
        auto root = EnvelopeBase();
        auto arr = json::Value::MkArr();
        for (const auto& w : f.windows)
        {
            if (!a.windowFilter.empty() && !IContains(w.windowId, a.windowFilter))
            {
                continue;
            }
            arr.Push(JWindow(f, w, true, false));
        }
        if (a.json)
        {
            root.Set(L"windows", std::move(arr));
            return EmitJson(root);
        }
        return CmdWindows(f, a); // human view is the same tree
    }

    int CmdList(const Fleet& f, const Args& a)
    {
        if (a.json)
        {
            auto root = EnvelopeBase();
            auto wins = json::Value::MkArr();
            for (const auto& w : f.windows)
            {
                wins.Push(JWindow(f, w, true, true));
            }
            root.Set(L"windows", std::move(wins));
            auto sess = json::Value::MkArr();
            for (const auto& s : f.sessions)
            {
                sess.Push(JSession(f, s, false, a.tail));
            }
            root.Set(L"sessions", std::move(sess));
            auto ext = json::Value::MkArr();
            for (const auto& lc : f.live)
            {
                if (lc.sessionId.empty() || f.sessionById.count(lc.sessionId) == 0)
                {
                    ext.Push(JExternal(f, lc));
                }
            }
            root.Set(L"external", std::move(ext));
            return EmitJson(root);
        }

        // human overview
        int liveCount = 0, archCount = 0;
        for (const auto& s : f.sessions)
        {
            (f.liveById.count(s.id) ? liveCount : archCount)++;
        }
        OutLn(L"Agentmaster fleet — profile: " + AgentmasterStateDir());
        OutLn(L"  windows: " + std::to_wstring(f.windows.size()) +
              L"   sessions: " + std::to_wstring(liveCount) + L" open / " + std::to_wstring(archCount) + L" archived" +
              L"   live claudes: " + std::to_wstring(f.live.size()));
        OutLn(L"");
        CmdWindows(f, a);
        OutLn(L"");
        OutLn(L"OPEN SESSIONS");
        Args sa = a;
        sa.archived = false;
        CmdSessions(f, sa);
        return 0;
    }

    int CmdShow(const Fleet& f, const Args& a)
    {
        std::wstring ref = a.ref;
        if (a.self)
        {
            const auto wt = EnvVar(L"WT_SESSION");
            if (wt.empty())
            {
                Err(L"--self: no WT_SESSION in environment (not inside a terminal tab?)");
                return 2;
            }
            // find the live claude sharing our WT_SESSION
            for (const auto& lc : f.live)
            {
                if (IEquals(lc.facts.wtSession, wt) && !lc.sessionId.empty())
                {
                    ref = lc.sessionId;
                    break;
                }
            }
            if (ref.empty())
            {
                Err(L"--self: no claude session found in this tab (WT_SESSION=" + wt + L")");
                return 1;
            }
        }
        if (ref.empty())
        {
            Err(L"show: a <ref> (session id / prefix / title) or --self is required");
            return 2;
        }

        std::vector<std::wstring> candidates;
        const int idx = ResolveSessionRef(f, ref, candidates);

        json::Value js;
        bool have = false;
        if (idx >= 0)
        {
            js = JSession(f, f.sessions[idx], true, a.tail);
            have = true;
        }
        else if (candidates.empty())
        {
            // Not managed in THIS profile — but it may be a LIVE claude (managed by the other
            // instance, or not yet persisted). A live claude always has a transcript, so synthesize
            // a record and reuse the rich JSession (the queue is simply empty).
            if (const LiveClaude* lc = FindLiveByRef(f, ref))
            {
                SessionInfo synth;
                synth.id = lc->sessionId;
                synth.workingDir = lc->facts.cwd;
                synth.external = true;
                const auto info = ReadTranscriptInfo(lc->facts.cwd, lc->sessionId, 65536, 1);
                synth.title = TranscriptDisplayTitle(info);
                synth.branch = info.gitBranch;
                js = JSession(f, synth, true, a.tail);
                have = true;
            }
        }
        if (!have)
        {
            if (candidates.empty())
            {
                Err(L"show: no session matches '" + ref + L"'");
            }
            else
            {
                Err(L"show: '" + ref + L"' is ambiguous — candidates:");
                for (const auto& c : candidates)
                {
                    Err(L"  " + c);
                }
            }
            return 1;
        }

        if (a.json)
        {
            auto root = EnvelopeBase();
            root.Set(L"session", js);
            return EmitJson(root);
        }

        // human detail (rendered from `js`, so it works for managed AND live-synthesized sessions)
        OutLn(L"╾─ " + Trunc(js.StrAt(L"title"), 80) + L" ──");
        OutLn(L"  id:        " + js.StrAt(L"id"));
        OutLn(L"  dir:       " + js.StrAt(L"workingDir") + (js.Find(L"liveCwd") ? L"   (live: " + js.StrAt(L"liveCwd") + L")" : L""));
        if (!js.StrAt(L"branch").empty())
        {
            OutLn(L"  branch:    " + js.StrAt(L"branch"));
        }
        OutLn(L"  state:     " + js.StrAt(L"derivedState") +
              (js.Find(L"presence") ? L"   presence=" + js.StrAt(L"presence") : L"") +
              (js.BoolAt(L"turnInFlight") ? L"   (turn in flight)" : L""));
        OutLn(L"  link:      " + js.StrAt(L"linkState") +
              (js.Find(L"pid") ? L"   pid=" + std::to_wstring(js.U32At(L"pid")) : L"") +
              (js.Find(L"model") ? L"   " + js.StrAt(L"model") : L"") +
              (js.Find(L"effort") ? L" · " + js.StrAt(L"effort") : L""));
        OutLn(L"  autopilot: " + js.StrAt(L"autopilot"));
        if (const auto* t = js.Find(L"timing"))
        {
            OutLn(L"  timing:    created " + t->StrAt(L"createdAgo") + L" ago · last activity " + t->StrAt(L"lastActivityAgo") + L" ago");
        }
        if (const auto* pl = js.Find(L"placement"))
        {
            OutLn(L"  placement: window " + pl->StrAt(L"windowId") + L" · tab " + std::to_wstring(pl->U32At(L"tabIndex")) + (pl->BoolAt(L"selected") ? L" (focused)" : L""));
        }
        if (const auto* act = js.Find(L"activity"))
        {
            OutLn(L"  activity:  " + std::to_wstring(act->U32At(L"messages")) + L" msgs · " + std::to_wstring(act->U32At(L"toolUses")) + L" tool calls");
        }
        if (js.Find(L"pendingInteractiveTool"))
        {
            OutLn(L"  ⚠ blocked on user — pending tool: " + js.StrAt(L"pendingInteractiveTool"));
        }
        if (js.Find(L"lastUserPrompt"))
        {
            OutLn(L"  you asked:  " + Trunc(js.StrAt(L"lastUserPrompt"), 120));
        }
        if (const auto* files = js.Find(L"filesTouched"); files && !files->arr.empty())
        {
            std::wstring fl;
            for (size_t i = 0; i < files->arr.size() && i < 6; ++i)
            {
                fl += (i ? L", " : L"") + Trunc(files->arr[i].AsStr(), 40);
            }
            OutLn(L"  files:     " + fl + (files->arr.size() > 6 ? L" (+" + std::to_wstring(files->arr.size() - 6) + L" more)" : L""));
        }

        // flight plan
        if (const auto* fp = js.Find(L"flightPlan"); fp && !fp->arr.empty())
        {
            OutLn(L"");
            OutLn(L"  FLIGHT PLAN");
            for (const auto& p : fp->arr)
            {
                OutLn(L"    [" + p.StrAt(L"status") + L"/" + p.StrAt(L"origin") + L"] " + Trunc(p.StrAt(L"text"), 90));
            }
        }

        // conversation
        if (const auto* conv = js.Find(L"conversation"); conv && !conv->arr.empty())
        {
            OutLn(L"");
            OutLn(L"  CONVERSATION (last " + std::to_wstring(conv->arr.size()) + L" turns)");
            for (const auto& t : conv->arr)
            {
                const auto role = t.StrAt(L"role");
                OutLn(L"  " + std::wstring(role == L"user" ? L"▷ you" : L"◆ claude") + L":");
                OutLn(L"    " + Trunc(t.StrAt(L"text"), 600));
            }
        }
        else if (js.Find(L"lastAssistantReply"))
        {
            OutLn(L"");
            OutLn(L"  LAST REPLY:");
            OutLn(L"    " + Trunc(js.StrAt(L"lastAssistantReply"), 600));
        }
        return 0;
    }

    int Usage()
    {
        OutLn(L"agentmaster — query the Claude session fleet (CLI.md)");
        OutLn(L"");
        OutLn(L"usage: agentmaster <verb> [args] [--json]");
        OutLn(L"  show <ref> [--tail N]   full introspection of one session (id/prefix/title, or --self)");
        OutLn(L"  list                    fleet overview: windows, tabs, sessions");
        OutLn(L"  sessions [--archived]   managed sessions (default: open only) [--state S] [--dir D]");
        OutLn(L"  tabs [--window W]       every tab across windows");
        OutLn(L"  windows                 saved windows: geometry, ordered tabs, lens");
        OutLn(L"  external                unmanaged live claudes (real-WT / bare console)");
        OutLn(L"");
        OutLn(L"global: --json  --self  --profile <dir>  --instance dev|release  --help");
        return 0;
    }
}

int wmain(int argc, wchar_t** argv)
{
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    // Emit UTF-8; make a real console interpret our bytes correctly (no effect when piped).
    ::SetConsoleOutputCP(CP_UTF8);

    Args a;
    std::wstring profileOverride;
    std::wstring instance;
    std::vector<std::wstring> positionals;

    for (int i = 1; i < argc; ++i)
    {
        const std::wstring arg = argv[i];
        auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : std::wstring{}; };
        if (arg == L"--json")
        {
            a.json = true;
        }
        else if (arg == L"--self")
        {
            a.self = true;
        }
        else if (arg == L"--archived")
        {
            a.archived = true;
        }
        else if (arg == L"--tail")
        {
            a.tail = ::_wtoi(next().c_str());
        }
        else if (arg == L"--state")
        {
            a.stateFilter = next();
        }
        else if (arg == L"--dir")
        {
            a.dirFilter = next();
        }
        else if (arg == L"--window")
        {
            a.windowFilter = next();
        }
        else if (arg == L"--profile")
        {
            profileOverride = next();
        }
        else if (arg == L"--instance")
        {
            instance = next();
        }
        else if (arg == L"--help" || arg == L"-h" || arg == L"-?")
        {
            return Usage();
        }
        else if (arg == L"--version")
        {
            OutLn(L"agentmaster-cli schema " + std::to_wstring(kSchemaVersion));
            return 0;
        }
        else if (!arg.empty() && arg[0] == L'-')
        {
            Err(L"unknown flag: " + arg);
            return 2;
        }
        else
        {
            positionals.push_back(arg);
        }
    }

    // Profile targeting MUST happen before any state read (ResolveProfileDir caches once).
    // Precedence (highest first):
    //   1. --profile <dir> / --instance dev|release        (explicit override)
    //   2. inherited AGENTMASTER_PROFILE env                (an agent INSIDE an app tab targets
    //                                                        its own instance automatically)
    //   3. MSIX package identity                            (a packaged dev exe -> dev profile,
    //                                                        release -> release; handled by
    //                                                        ResolveProfileDir, so do nothing)
    //   4. compile-time brand (AGENTMASTER_DEV)             (an UNPACKAGED dev build run OUTSIDE
    //                                                        any app defaults to ~/.agentmaster-dev)
    //   5. the per-identity default (~/.agentmaster)        (ResolveProfileDir fallback)
    if (!profileOverride.empty())
    {
        ::SetEnvironmentVariableW(L"AGENTMASTER_PROFILE", profileOverride.c_str());
    }
    else if (!instance.empty())
    {
        const auto home = EnvVar(L"USERPROFILE");
        if (!home.empty())
        {
            const std::wstring dir = home + (IEquals(instance, L"dev") ? L"\\.agentmaster-dev" : L"\\.agentmaster");
            ::SetEnvironmentVariableW(L"AGENTMASTER_PROFILE", dir.c_str());
        }
    }
    else if (IsPackaged())
    {
        // Packaged: OUR package identity is the authoritative "which install am I" signal — the user
        // chose it by typing `agentmasterdev` vs `agentmaster`. CLEAR any AGENTMASTER_PROFILE
        // inherited from a shell hosted in the OTHER instance, so ResolveProfileDir resolves via
        // GetCurrentPackageFamilyName + the saved choice (dev exe -> dev profile, release ->
        // release), never the ambient env. The saved custom-folder choice (the picker) still wins —
        // it lives in .agentmaster.profiles keyed by identity, which that resolution consults.
        ::SetEnvironmentVariableW(L"AGENTMASTER_PROFILE", nullptr);
    }
#ifdef AGENTMASTER_DEV
    else if (EnvVar(L"AGENTMASTER_PROFILE").empty())
    {
        // Unpackaged dev build with no inherited profile — default to the dev profile so "the binary
        // targets its build type". (A packaged build never reaches here; identity handled it above.)
        const auto home = EnvVar(L"USERPROFILE");
        if (!home.empty())
        {
            ::SetEnvironmentVariableW(L"AGENTMASTER_PROFILE", (home + L"\\.agentmaster-dev").c_str());
        }
    }
#endif

    if (positionals.empty())
    {
        return Usage();
    }
    a.verb = positionals[0];
    if (positionals.size() > 1)
    {
        a.ref = positionals[1];
    }

    const Fleet fleet = GatherFleet();

    if (a.verb == L"show")
    {
        return CmdShow(fleet, a);
    }
    if (a.verb == L"list")
    {
        return CmdList(fleet, a);
    }
    if (a.verb == L"sessions")
    {
        return CmdSessions(fleet, a);
    }
    if (a.verb == L"tabs")
    {
        return CmdTabs(fleet, a);
    }
    if (a.verb == L"windows")
    {
        return CmdWindows(fleet, a);
    }
    if (a.verb == L"external")
    {
        return CmdExternal(fleet, a);
    }

    Err(L"unknown verb: " + a.verb);
    Usage();
    return 2;
}
