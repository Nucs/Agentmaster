// Agentmaster - rewind_probe (tools\interrupt-probe\README.md, doc\agentmaster\HOOKS.md "Interrupted turns").
//
// Runs the REAL C++ planner / rewind / classifier of ApiErrorRewind.cpp on a transcript file, so the probes can
// check them against transcripts a real claude.exe wrote (cpp_probe.py drives it). Built by build_rewind_probe.bat
// from the objects of a built m5 harness, which must come from the same sources as the ApiErrorRewind.h it includes.
//
//   rewind_probe plan <transcript> <sid>      PlanInterruptRewind on the whole file (prints the verdict + the line)
//   rewind_probe apply <transcript> <sid>     RewindInterruptTail: plan, the holder check, the append (exit 0 = appended)
//   rewind_probe classify <transcript>        ClassifyResumeContinuation on the whole file
//
// ⚠ apply WRITES to the transcript it is given: point it only at a scratch transcript (the probes' CLAUDE_CONFIG_DIR).
#include "ApiErrorRewind.h"

#include <windows.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using namespace Agentmaster;

// Reads a whole file as bytes.
//   path: the file.
//   returns: its bytes, or "" when it cannot be opened (the planner then reports an empty transcript).
static std::string ReadAll(const wchar_t* path)
{
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// The enumerator's name, for the printout.
//   v: a planner verdict.
//   returns: its name, or "?" for a value this probe does not know (a newer planner).
static const wchar_t* VerdictName(InterruptRewindVerdict v)
{
    switch (v)
    {
    case InterruptRewindVerdict::NotApplicable:
        return L"NotApplicable";
    case InterruptRewindVerdict::Rewind:
        return L"Rewind";
    case InterruptRewindVerdict::WouldNotContinue:
        return L"WouldNotContinue";
    case InterruptRewindVerdict::UnknownShape:
        return L"UnknownShape";
    case InterruptRewindVerdict::NeedMoreHistory:
        return L"NeedMoreHistory";
    case InterruptRewindVerdict::TornTail:
        return L"TornTail";
    }
    return L"?";
}

// The enumerator's name, for the printout.
//   v: a classifier verdict.
//   returns: its name, or "?" for a value this probe does not know (a newer classifier).
static const wchar_t* ContinuationName(ResumeContinuation v)
{
    switch (v)
    {
    case ResumeContinuation::None:
        return L"None";
    case ResumeContinuation::ContinuesTurn:
        return L"ContinuesTurn";
    case ResumeContinuation::ResendsPrompt:
        return L"ResendsPrompt";
    case ResumeContinuation::NeedMoreHistory:
        return L"NeedMoreHistory";
    }
    return L"?";
}

// Runs one mode on one transcript.
//   argv[1]: plan | apply | classify;  argv[2]: the transcript;  argv[3]: the session id (plan, apply).
//   returns: 0 = done (apply: appended), 1 = apply did not append, 2 = a usage error.
//   Throws nothing on purpose: the planner and the rewind report every failure in their results, and an
//   allocation failure ends the probe the way any small tool ends.
int wmain(int argc, wchar_t** argv)
{
    if (argc < 3)
    {
        std::fwprintf(stderr, L"usage: rewind_probe plan|apply|classify <transcript> [sid]\n");
        return 2;
    }
    const std::wstring mode = argv[1];
    const std::wstring path = argv[2];
    const std::wstring sid = argc > 3 ? argv[3] : L"";
    if (mode == L"plan")
    {
        // windowIsWholeFile = true: the bytes start at byte 0, so a walk that leaves them is the history's real
        // start, never a request for more history (NeedMoreHistory).
        const auto p = PlanInterruptRewind(ReadAll(path.c_str()), true, sid);
        std::wprintf(L"verdict=%s target=%s kind=%s text=%s duringToolUse=%d partial=%zu discarded=%zu continueRows=%zu errors=%zu after=%zu reason=%s\n",
                     VerdictName(p.verdict), p.targetUuid.c_str(), p.targetKind.c_str(), p.targetText.c_str(), p.duringToolUse ? 1 : 0,
                     p.partialRows, p.discardedResults, p.resumePromptRows, p.errorRows, p.rowsAfterInterrupt, p.reason.c_str());
        // The marker line is UTF-8 bytes: printed narrow, never through the wide stream.
        std::printf("marker=%s\n", p.markerLine.c_str());
        return 0;
    }
    if (mode == L"apply")
    {
        // The app's own entry point: it refuses while a live heartbeat names the conversation (the exit
        // re-stamp rule), then appends exactly the planned line.
        const auto r = RewindInterruptTail(path, sid);
        std::wprintf(L"%s\n", DescribeInterruptRewind(r, sid, L"probe").c_str());
        return r.outcome == ApiErrorRewindOutcome::Appended ? 0 : 1;
    }
    if (mode == L"classify")
    {
        // The whole file again (windowIsWholeFile = true), as above.
        const auto c = ClassifyResumeContinuation(ReadAll(path.c_str()), true);
        std::wprintf(L"verdict=%s tail=%s prompt=%s dropped=%s reason=%s\n", ContinuationName(c.verdict), c.tailKind.c_str(), c.promptText.c_str(), c.droppedTool.c_str(), c.reason.c_str());
        return 0;
    }
    std::fwprintf(stderr, L"unknown mode: %s\n", mode.c_str());
    return 2;
}
