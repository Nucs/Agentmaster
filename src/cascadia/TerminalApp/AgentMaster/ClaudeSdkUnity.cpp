// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the ONE translation unit that compiles the Claude Agent SDK into the app
// (SDK_HARNESS.md §2).
//
// The SDK is the git submodule refs/claude-agent-sdk-cpp (0xeb/claude-agent-sdk-cpp, MIT), pinned
// to one commit. It is a CMake project; Agentmaster is MSBuild. Instead of a second project file
// that would have to mirror the SDK's source list and flags, this file includes the SDK's .cpp
// files directly ("unity build"). The consequences:
//
//   - WHICH FILES. Only what ClaudeClient needs: version, errors, types, the control protocol,
//     the message parser, the client and the subprocess transport. NOT included:
//       * src/internal/subprocess/process.cpp — Agentmaster supplies its own Windows backend of
//         that interface (ClaudeSdkProcessWin32.cpp explains the three defects it avoids);
//       * query.cpp, sessions/, ext/, testing/ — the one-shot query API and the session-store
//         subsystem. Nothing here calls them. (The session mirror they serve does not work with
//         --no-session-persistence: SDK_HARNESS.md §6.)
//     A file added here later must be checked the same way: `cl /Zs` with the app's flags.
//   - WARNINGS. The app compiles at /W3 /WX /sdl with five more warnings as errors. The SDK is
//     written for other settings, so its code is compiled inside a warning(push)/(pop) region
//     with exactly the warnings it raises turned off (the list below names each). Agentmaster's
//     own code never goes inside that region.
//   - std::atexit. subprocess_transport.cpp registers an atexit handler that ends its child
//     processes. In this build the handler is useless and unsafe:
//       * the app ends with TerminateProcess (WindowEmperor), so no atexit handler ever runs;
//       * in a program that DOES exit normally (the live test), the handler runs AFTER the
//         function-local statics it reads were destroyed (they are constructed later than the
//         handler is registered, so they are destroyed earlier) — it would walk a destroyed
//         std::set.
//     Children are ended by the kill-on-close job object of ClaudeSdkProcessWin32.cpp instead. So
//     the one registration is redirected to std::at_quick_exit (a real library function with the
//     same signature, whose handlers run only on std::quick_exit, which nothing calls).
//   - ORDER. subprocess_transport.cpp comes LAST: it includes <windows.h>, and its macros must
//     not reach the other SDK files, which the SDK builds without them.
//
// The SDK's public headers need <nlohmann/json.hpp>: the vcpkg port nlohmann-json (vcpkg.json,
// feature "terminal"). Include paths this file needs (TerminalAppLib.vcxproj sets them for this
// file and for the two other files that include SDK headers):
//   refs/claude-agent-sdk-cpp/include   and   refs/claude-agent-sdk-cpp/src
//
// After every move of the submodule pin: SDK_HARNESS.md §9.
//
// Plain C++, no WinRT (the vcxproj marks it <PrecompiledHeader>NotUsing</PrecompiledHeader>).

#if !__has_include("../../../../refs/claude-agent-sdk-cpp/src/client.cpp")
#error "The Claude Agent SDK submodule is not checked out. Run:  git submodule update --init refs/claude-agent-sdk-cpp"
#endif

#if !__has_include(<nlohmann/json.hpp>)
#error "nlohmann/json.hpp was not found. It comes from the vcpkg port nlohmann-json (vcpkg.json, feature \"terminal\"): run a full build once so vcpkg installs it."
#endif

// Every standard header the SDK files include, BEFORE the atexit redirect below, so that no
// library declaration is ever read with the macro in effect. (They are include-guarded: the SDK
// files' own #include lines then do nothing.)
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#pragma warning(push)
// The SDK's warnings under the app's flags, each seen in a `cl /Zs` run of the pinned commit:
//   4099 — transport.hpp says `class ClaudeOptions;`, types.hpp defines `struct ClaudeOptions`.
//   4100 — unreferenced parameters (an error here through /we4100).
//   4996 — std::getenv (an error here through /sdl).
// And, not raised today but errors here by /we or /sdl, so that a small SDK change cannot break
// the build for a reason that is not a defect: 4189 (unused local), 4242 / 4244 / 4267 (narrowing
// conversions), 4389 (signed/unsigned ==), 4456-4459 (shadowing), 4701 / 4703 (maybe-uninitialized).
#pragma warning(disable : 4099 4100 4189 4242 4244 4267 4389 4456 4457 4458 4459 4701 4703 4996)

#include "../../../../refs/claude-agent-sdk-cpp/src/version.cpp"
#include "../../../../refs/claude-agent-sdk-cpp/src/errors.cpp"
#include "../../../../refs/claude-agent-sdk-cpp/src/types.cpp"
#include "../../../../refs/claude-agent-sdk-cpp/src/protocol/control.cpp"
#include "../../../../refs/claude-agent-sdk-cpp/src/internal/message_parser.cpp"
#include "../../../../refs/claude-agent-sdk-cpp/src/client.cpp"

// <windows.h> is what subprocess_transport.cpp includes next; read it now, before the redirect.
#include <windows.h>

// See "std::atexit" in the file header. The macro rewrites every `atexit(` that the compiler reads
// while it is defined — a call AND a declaration. That is why every header the SDK file includes
// was read above, with the macro not yet defined: only the SDK file's own code sees it, and that
// code holds exactly one `std::atexit(...)` call.
#define atexit(handler) at_quick_exit(handler)
#include "../../../../refs/claude-agent-sdk-cpp/src/internal/transport/subprocess_transport.cpp"
#undef atexit

#pragma warning(pop)
