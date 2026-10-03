// Agentmaster: devidle — what does merely HOLDING a Direct3D 11 device cost on this machine's GPU
// driver, does a device that paints nothing wake up when ANOTHER device in the same process presents
// frames, and does a swap chain that nothing shows (a hidden tab's) get throttled?
// The evidence behind doc/agentmaster/RENDERING.md §4b.
//
//   devidle <devices> <swapchain 0|1> <painting> <fps> <seconds> [settleSeconds=2]
//
// Each device is created exactly like AtlasEngine creates one per terminal (AtlasEngine.r.cpp
// _recreateBackend: D3D11_CREATE_DEVICE_SINGLETHREADED | PREVENT_INTERNAL_THREADING_OPTIMIZATIONS |
// BGRA_SUPPORT on adapter 0). With swapchain=1 each device also gets AtlasEngine's swap chain
// (_createSwapChain: 1602x969 BGRA, 3 buffers, FLIP_SEQUENTIAL, a frame-latency waitable object,
// maximum frame latency 1) on a DirectComposition surface handle that no visual shows — the state of
// a terminal whose tab is not selected — presented once. The first <painting> devices then paint at
// <fps>: wait on the frame-latency object (100 ms cap), clear, Present1 with a one-row dirty rect —
// the Renderer + AtlasEngine frame shape. The rest stay idle.
//
// After <settleSeconds> the process measures its OWN threads for <seconds>: wake-ups (context
// switches) and CPU per Win32 start module, and per NVIDIA device (the 20 nvwgf2umx.dll threads each
// device starts, cut by creation order).
//
// Measured 2026-10-03 (RTX 4060 Ti, driver 32.0.16.1656, 60 Hz display):
//   - every device keeps 2 NVIDIA threads waking ~68 times/s, painting or not, with or without a swap
//     chain, after 2 s, 150 s or 15 min of idleness, under any exe name (WindowsTerminal.exe and
//     Agentmaster.exe copies behave the same); a painting device adds ~1 wake per frame;
//   - a swap chain nothing shows still presents at the requested rate (59/s at 60 requested): the
//     frame-latency wait does not throttle a hidden terminal; clear + Present1 is ~0.5 ms CPU/frame.
//
// Safety: it creates <devices> hardware devices in ITS OWN process only. Keep <devices> small: the
// ~62-device limit in RENDERING.md §1 is per process, and that test (hundreds of devices) provokes
// real driver hang-recoveries — ask before running it. It never touches another process.
//
// Build: tools\_build_devidle.bat (vcvars64 + cl; links d3d11 / dxgi / dcomp).

// windows.h defines min/max macros, which break std::min / std::max below.
#define NOMINMAX
#include <windows.h>
#include <d3d11_2.h>
#include <dcomp.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")

using Microsoft::WRL::ComPtr;

// Abort the run with the failing call's text + HRESULT. A measurement has no recovery story: a
// partial setup would measure something other than AtlasEngine's shape, so every graphics call is
// checked and any failure exits with code 1.
#define CHECK(x)                                                                         \
    do                                                                                   \
    {                                                                                    \
        const HRESULT hr_ = (x);                                                         \
        if (FAILED(hr_))                                                                 \
        {                                                                                \
            std::fprintf(stderr, "%s failed: 0x%08lX (line %d)\n", #x, hr_, __LINE__); \
            std::exit(1);                                                                \
        }                                                                                \
    } while (0)

// NtQuerySystemInformation / NtQueryInformationThread, resolved from ntdll at run time (no ntdll.lib).
using NtQuerySystemInformationFn = LONG(WINAPI*)(ULONG, PVOID, ULONG, PULONG);
using NtQueryInformationThreadFn = LONG(WINAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

// One thread's counters in one snapshot: total CPU (100 ns units) and its context-switch counter.
struct ThreadSample
{
    LONGLONG cpu100ns;
    ULONG contextSwitches;
};

// One device under test: the device, its context, and — with swapchain=1 — AtlasEngine's swap chain,
// its render target view, its frame-latency event and the composition surface handle it presents to.
// Nothing is released explicitly: the process exits right after the measurement.
struct Terminal
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain2> swapChain;
    ComPtr<ID3D11RenderTargetView> rtv;
    HANDLE latency = nullptr;
    HANDLE surface = nullptr;
};

// Snapshots THIS process's threads from SystemProcessInformation (class 5).
// x64 layout: SYSTEM_PROCESS_INFORMATION is 256 bytes (UniqueProcessId at +80), followed by its
// SYSTEM_THREAD_INFORMATION array (80 bytes each: KernelTime +0, UserTime +8, ClientId.UniqueThread
// +48, ContextSwitches +64).
// Arguments:
// - ntqsi: the resolved NtQuerySystemInformation.
// Return Value:
// - per thread id, its sample; EMPTY when the query fails (the caller then prints no rows — a
//   visible failure, never a wrong number).
static std::map<DWORD, ThreadSample> SnapOwnThreads(NtQuerySystemInformationFn ntqsi)
{
    std::map<DWORD, ThreadSample> out;
    std::vector<BYTE> buf(8 << 20);
    ULONG ret = 0;
    LONG status;
    // STATUS_INFO_LENGTH_MISMATCH: the system's process list outgrew the buffer — grow and retry.
    while ((status = ntqsi(5, buf.data(), static_cast<ULONG>(buf.size()), &ret)) == static_cast<LONG>(0xC0000004))
    {
        buf.resize(buf.size() * 2);
    }
    if (status != 0)
    {
        return out;
    }
    const auto self = static_cast<ULONG_PTR>(GetCurrentProcessId());
    size_t off = 0;
    for (;;)
    {
        const BYTE* p = buf.data() + off;
        const ULONG next = *reinterpret_cast<const ULONG*>(p);
        const ULONG nthreads = *reinterpret_cast<const ULONG*>(p + 4);
        const ULONG_PTR pid = *reinterpret_cast<const ULONG_PTR*>(p + 80);
        if (pid == self)
        {
            for (ULONG i = 0; i < nthreads; ++i)
            {
                const BYTE* t = p + 256 + static_cast<size_t>(i) * 80;
                const LONGLONG k = *reinterpret_cast<const LONGLONG*>(t + 0);
                const LONGLONG u = *reinterpret_cast<const LONGLONG*>(t + 8);
                const DWORD tid = static_cast<DWORD>(*reinterpret_cast<const ULONG_PTR*>(t + 48));
                const ULONG csw = *reinterpret_cast<const ULONG*>(t + 64);
                out[tid] = { k + u, csw };
            }
            break;
        }
        if (next == 0)
        {
            break;
        }
        off += next;
    }
    return out;
}

// Names the module that contains an address in THIS process.
// Arguments:
// - addr: the address (a thread's Win32 start address).
// Return Value:
// - the module's file name without its folder, or L"?" when no module contains it.
static std::wstring ModuleOf(void* addr)
{
    HMODULE h = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(addr), &h))
    {
        return L"?";
    }
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(h, path, MAX_PATH);
    const wchar_t* leaf = wcsrchr(path, L'\\');
    return leaf ? leaf + 1 : path;
}

// Creates one device the way AtlasEngine does, and (optionally) its swap chain, presented once.
// Arguments:
// - adapter: adapter 0 (AtlasEngine's choice unless WARP is forced).
// - media: the factory interface that creates swap chains on composition surface handles.
// - withSwapChain: also create + present the swap chain.
// - width, height: the swap chain size in pixels (the release's real terminal size by default).
// Return Value:
// - the device under test. Any failed call exits the process (CHECK).
static Terminal CreateTerminal(IDXGIAdapter1* adapter, IDXGIFactoryMedia* media, bool withSwapChain, UINT width, UINT height)
{
    Terminal t;
    constexpr UINT flags = D3D11_CREATE_DEVICE_SINGLETHREADED | D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    static constexpr D3D_FEATURE_LEVEL levels[]{ D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    CHECK(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, 2, D3D11_SDK_VERSION, &t.device, nullptr, &t.context));
    if (!withSwapChain)
    {
        return t;
    }

    // COMPOSITIONSURFACE_ALL_ACCESS (0x0003): the same request AtlasEngine makes.
    CHECK(DCompositionCreateSurfaceHandle(0x0003L, nullptr, &t.surface));
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 3;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    ComPtr<IDXGISwapChain1> sc1;
    CHECK(media->CreateSwapChainForCompositionSurfaceHandle(t.device.Get(), t.surface, &desc, nullptr, &sc1));
    CHECK(sc1.As(&t.swapChain));
    CHECK(t.swapChain->SetMaximumFrameLatency(1));
    t.latency = t.swapChain->GetFrameLatencyWaitableObject();

    // In D3D11 flip model, buffer 0 always names the CURRENT back buffer, so one view serves every
    // frame (AtlasEngine keeps one _renderTargetView the same way and rebinds it after each Present).
    ComPtr<ID3D11Texture2D> buffer;
    CHECK(t.swapChain->GetBuffer(0, IID_PPV_ARGS(&buffer)));
    CHECK(t.device->CreateRenderTargetView(buffer.Get(), nullptr, &t.rtv));

    WaitForSingleObjectEx(t.latency, 100, TRUE);
    static constexpr float black[4]{ 0, 0, 0, 1 };
    t.context->OMSetRenderTargets(1, t.rtv.GetAddressOf(), nullptr);
    t.context->ClearRenderTargetView(t.rtv.Get(), black);
    CHECK(t.swapChain->Present(1, 0));
    return t;
}

// Paints one device at a fixed rate until told to stop: the Renderer thread's loop shape.
// Arguments:
// - t: the device (must have a swap chain).
// - fps: frames per second to attempt.
// - width: the swap chain width, for the one-row dirty rect.
// - stop: set to true to end the loop.
// - frames: incremented once per presented frame (the achieved rate is read from it).
static void PaintLoop(Terminal& t, int fps, UINT width, const std::atomic<bool>& stop, std::atomic<long long>& frames)
{
    const auto period = std::chrono::microseconds(1000000 / fps);
    auto next = std::chrono::steady_clock::now();
    float shade = 0;
    while (!stop.load())
    {
        // AtlasEngine::_waitUntilCanRender: at most 100 ms, so a surface nobody consumes would cap
        // the loop near 10 frames/s. The measurement shows whether that cap ever applies.
        WaitForSingleObjectEx(t.latency, 100, TRUE);
        shade = shade > 0.9f ? 0 : shade + 0.05f;
        const float color[4]{ shade, 0, 0, 1 };
        t.context->OMSetRenderTargets(1, t.rtv.GetAddressOf(), nullptr);
        t.context->ClearRenderTargetView(t.rtv.Get(), color);
        RECT dirty{ 0, 0, static_cast<LONG>(width), 20 };
        DXGI_PRESENT_PARAMETERS params{ 1, &dirty, nullptr, nullptr };
        t.swapChain->Present1(1, 0, &params);
        frames.fetch_add(1);
        next += period;
        std::this_thread::sleep_until(next);
    }
}

// Runs the experiment described in the file header and prints its result.
// Arguments:
// - argc, argv: <devices> <swapchain 0|1> <painting> <fps> <seconds> [settleSeconds=2].
// Return Value:
// - 0 after a measurement, 2 on a usage error; 1 (via CHECK) when a graphics call fails.
int wmain(int argc, wchar_t** argv)
{
    if (argc < 6)
    {
        std::fprintf(stderr, "usage: devidle <devices> <swapchain 0|1> <painting> <fps> <seconds> [settleSeconds=2]\n");
        return 2;
    }
    const int devices = std::max(1, _wtoi(argv[1]));
    const bool withSwapChain = _wtoi(argv[2]) != 0;
    const int painting = withSwapChain ? std::clamp(_wtoi(argv[3]), 0, devices) : 0;
    const int fps = std::clamp(_wtoi(argv[4]), 1, 240);
    const int seconds = std::max(1, _wtoi(argv[5]));
    // How long the devices sit (idle or painting) before the window opens. A long settle tells a
    // driver that backs off after inactivity from one that polls forever.
    const int settleSeconds = argc > 6 ? std::max(0, _wtoi(argv[6])) : 2;
    constexpr UINT width = 1602, height = 969;

    const auto ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto ntqsi = reinterpret_cast<NtQuerySystemInformationFn>(GetProcAddress(ntdll, "NtQuerySystemInformation"));
    const auto ntqit = reinterpret_cast<NtQueryInformationThreadFn>(GetProcAddress(ntdll, "NtQueryInformationThread"));

    ComPtr<IDXGIFactory2> factory;
    CHECK(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;
    CHECK(factory->EnumAdapters1(0, &adapter));
    ComPtr<IDXGIFactoryMedia> media;
    CHECK(factory.As(&media));

    std::vector<Terminal> terms;
    terms.reserve(devices);
    for (int i = 0; i < devices; ++i)
    {
        terms.push_back(CreateTerminal(adapter.Get(), media.Get(), withSwapChain, width, height));
    }

    std::atomic<bool> stop{ false };
    std::atomic<long long> frames{ 0 };
    std::vector<std::thread> painters;
    for (int i = 0; i < painting; ++i)
    {
        painters.emplace_back([&, i] { PaintLoop(terms[i], fps, width, stop, frames); });
    }

    Sleep(static_cast<DWORD>(settleSeconds) * 1000);
    const auto a = SnapOwnThreads(ntqsi);
    const long long f0 = frames.load();
    const auto t0 = GetTickCount64();
    Sleep(static_cast<DWORD>(seconds) * 1000);
    const auto b = SnapOwnThreads(ntqsi);
    const double secs = (GetTickCount64() - t0) / 1000.0;
    const long long f1 = frames.load();

    struct Row
    {
        std::wstring module;
        FILETIME created;
        double cpuMsPerS;
        double wakesPerS;
    };
    std::vector<Row> rows;
    for (const auto& [tid, sa] : a)
    {
        const auto it = b.find(tid);
        if (it == b.end())
        {
            continue; // exited inside the window: a partial delta would mislead
        }
        Row r{ L"?", {}, (it->second.cpu100ns - sa.cpu100ns) / 10000.0 / secs, (it->second.contextSwitches - sa.contextSwitches) / secs };
        if (const auto h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, tid))
        {
            void* start = nullptr;
            // Class 9 = ThreadQuerySetWin32StartAddress: the routine CreateThread was given.
            if (ntqit(h, 9, &start, sizeof(start), nullptr) == 0)
            {
                r.module = ModuleOf(start);
            }
            FILETIME exitTime, kernel, user;
            GetThreadTimes(h, &r.created, &exitTime, &kernel, &user);
            CloseHandle(h);
        }
        rows.push_back(r);
    }

    std::printf("devices=%d swapchain=%d painting=%d fps=%d settle=%ds window=%.1fs frames/s=%.1f\n",
                devices, withSwapChain ? 1 : 0, painting, fps, settleSeconds, secs, (f1 - f0) / secs);
    std::map<std::wstring, std::tuple<int, int, double, double>> byModule;
    for (const auto& r : rows)
    {
        auto& [n, active, cpu, wakes] = byModule[r.module];
        ++n;
        active += r.wakesPerS > 0 ? 1 : 0;
        cpu += r.cpuMsPerS;
        wakes += r.wakesPerS;
    }
    for (const auto& [module, v] : byModule)
    {
        const auto& [n, active, cpu, wakes] = v;
        std::printf("  %-28ls threads %3d active %3d  cpu %7.2f ms/s  wakes %8.1f /s\n", module.c_str(), n, active, cpu, wakes);
    }

    // The NVIDIA threads in creation order, 20 per device: the per-device view.
    std::vector<Row> nvidia;
    for (const auto& r : rows)
    {
        if (_wcsicmp(r.module.c_str(), L"nvwgf2umx.dll") == 0)
        {
            nvidia.push_back(r);
        }
    }
    std::sort(nvidia.begin(), nvidia.end(), [](const Row& x, const Row& y) { return CompareFileTime(&x.created, &y.created) < 0; });
    for (size_t i = 0; i < nvidia.size(); i += 20)
    {
        int active = 0;
        double wakes = 0;
        for (size_t j = i; j < std::min(nvidia.size(), i + 20); ++j)
        {
            active += nvidia[j].wakesPerS > 0 ? 1 : 0;
            wakes += nvidia[j].wakesPerS;
        }
        const bool paints = static_cast<int>(i / 20) < painting;
        std::printf("  device %zu (%s): nv threads active %d, wakes %.1f /s\n", i / 20, paints ? "painting" : "idle", active, wakes);
    }

    stop = true;
    for (auto& p : painters)
    {
        p.join();
    }
    return 0;
}
