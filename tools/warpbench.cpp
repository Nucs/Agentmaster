// Agentmaster: warpbench — what one terminal costs when it is drawn on the CPU (Direct3D's WARP
// rasterizer) instead of the GPU. The evidence behind the "Software rendering" setting
// (doc/agentmaster/RENDERING.md); re-run it when the terminal size, DPI or font changes.
//
//   warpbench <cols> <rows> <cellW> <cellH> <fillPercent> <colorRunsPerRow> <frames> [hw|warp|both]
//       CPU per FULL FRAME, GPU vs WARP. Reproduces AtlasEngine's Direct2D backend
//       (src/renderer/atlas/BackendD2D.cpp — what WARP uses: AtlasEngine maps Automatic to Direct2D on a
//       software adapter), which REDRAWS THE WHOLE SCREEN EVERY FRAME no matter how little changed:
//         1. fill the entire target from a (cols+2)x(rows+2) background bitmap, upscaled by a
//            nearest-neighbor bitmap brush with D2D1_PRIMITIVE_BLEND_COPY (BackendD2D::_drawBackground);
//         2. draw every row as one DrawGlyphRun per foreground-color run (BackendD2D::_drawText),
//            grayscale antialiasing (Terminal's default), Cascadia Mono (else Consolas).
//       The text is synthetic: `fillPercent` of each row is printable ASCII (the rest spaces), split
//       into `colorRunsPerRow` colors. A busy Claude screen is ~60% / 4 runs, a quiet one ~15% / 2.
//       Each frame is completed with an event query so the work WARP does on its own worker threads
//       is counted. Report "CPU/frame" (this whole process: user + kernel, every thread). "wall/frame"
//       is dominated by that per-frame completion wait (~15 ms for WARP, a timer quantum) and does
//       NOT say how fast the real renderer paints.
//
//   warpbench mem <count> <widthPx> <heightPx>
//       RAM held per software-rendered terminal: <count> x (WARP device + AtlasEngine's swap chain —
//       3 buffers, FLIP_SEQUENTIAL, on a DirectComposition surface handle — each buffer written once).
//       Reported as SYSTEM COMMIT, not process private bytes: flip-model buffers are sections shared
//       with DWM, so they never show up in the owning process's private bytes.
//
// Measured 2026-10-01 (i9-13900K, RTX 4060 Ti, 178x51 cells = 1,602x969 px — Cascadia Mono 12 pt at
// 96 DPI in a 1920x1032 window; the machine was 45–60% busy with other work, hence the spread over
// repeated runs — WARP threads that land on E-cores or share a busy core cost more):
//   GPU   0.10–0.39 ms CPU per frame
//   WARP  1.0–2.8 ms (15% fill, 2 runs) / 1.9–5.9 ms (60%, 4 runs) / 5.2 ms (90%, 8 runs) CPU per frame
//   RAM   ~19 MB of system commit per WARP terminal (3 x 6.2 MB buffers + ~1.2 MB private)
//
// Safety: `hw` creates ONE hardware device in THIS process — harmless (the per-process ~62-device
// limit RENDERING.md describes is per process). Never point this at a running Agentmaster: it is a
// standalone process and touches nothing else.
//
// Build: tools\_build_warpbench.bat (vcvars64 + cl; links d3d11 / d2d1 / dwrite / dxgi / dcomp / psapi).

#include <windows.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <psapi.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "psapi.lib")

using Microsoft::WRL::ComPtr;

// Abort the run with the failing call's text + HRESULT. A benchmark has no recovery story: a partial
// measurement is worse than none, so every graphics call is checked and any failure exits with code 1.
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

// This process's total CPU time (user + kernel, all threads — WARP rasterizes on its own worker
// threads, so a single-thread clock would miss most of the cost).
// Returns: milliseconds since process start. Never throws.
static double ProcessCpuMs()
{
    FILETIME created{}, exited{}, kernel{}, user{};
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    const auto toMs = [](const FILETIME f) {
        return static_cast<double>((static_cast<unsigned long long>(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 10000.0;
    };
    return toMs(kernel) + toMs(user);
}

// System-wide committed memory — the only counter that sees flip-model swap-chain buffers (DWM-shared
// sections). Other processes move it too, so `mem` measures many terminals at once to drown the noise.
// Returns: megabytes committed system-wide. Never throws.
static double CommitMb()
{
    PERFORMANCE_INFORMATION pi{};
    GetPerformanceInfo(&pi, sizeof(pi));
    return static_cast<double>(pi.CommitTotal) * pi.PageSize / 1048576.0;
}

// This process's private bytes (the device / D2D / glyph-cache share of a terminal's RAM).
// Returns: megabytes. Never throws.
static double PrivateMb()
{
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    return static_cast<double>(pmc.PrivateUsage) / 1048576.0;
}

// One synthetic terminal screen, pre-shaped: glyph indices per row and the color runs covering it.
struct Screen
{
    std::vector<std::vector<UINT16>> glyphs; // [row][col] glyph index (spaces included, like AtlasEngine)
    std::vector<std::vector<int>> runLengths; // [row] lengths of the foreground-color runs (sum == cols)
};

// Builds a deterministic (fixed seed) screen so GPU and WARP draw identical frames.
// Parameters: face — the font face to map characters through; cols/rows — the grid; fillPercent —
// the share of each row holding printable characters; runsPerRow — how many color runs split each row.
// Returns: the shaped screen. Exits the process on a DirectWrite failure (CHECK).
static Screen MakeScreen(IDWriteFontFace* face, const int cols, const int rows, const int fillPercent, const int runsPerRow)
{
    Screen s;
    srand(1234);
    const std::wstring alphabet = L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789(){}[]<>=+-*/.,:;'\"_|\\";
    for (int y = 0; y < rows; ++y)
    {
        std::vector<UINT32> codepoints(cols);
        const int filled = cols * fillPercent / 100;
        for (int x = 0; x < cols; ++x)
        {
            // ~1 in 7 cells of the filled part stays a space: word breaks, like real text.
            codepoints[x] = (x < filled && (rand() % 7) != 0) ? alphabet[rand() % alphabet.size()] : L' ';
        }
        std::vector<UINT16> indices(cols);
        CHECK(face->GetGlyphIndices(codepoints.data(), cols, indices.data()));
        s.glyphs.push_back(std::move(indices));
        std::vector<int> runs;
        int left = cols;
        for (int r = 0; r < runsPerRow - 1 && left > 1; ++r)
        {
            const int len = 1 + rand() % (left / 2 + 1);
            runs.push_back(len);
            left -= len;
        }
        runs.push_back(left);
        s.runLengths.push_back(std::move(runs));
    }
    return s;
}

// Times `frames` full BackendD2D-shaped frames on one device and prints wall + CPU per frame.
// Parameters: label — the row's tag; warp — WARP (true) or the default hardware adapter (false);
// cols/rows/cellW/cellH — the grid and its cell size in pixels; fillPercent/runsPerRow — see
// MakeScreen; frames — how many timed frames (10 untimed warm-up frames run first: glyph cache,
// WARP's shader JIT). Exits the process on any graphics failure (CHECK).
static void Bench(const char* label, const bool warp, const int cols, const int rows, const int cellW, const int cellH,
                  const int fillPercent, const int runsPerRow, const int frames)
{
    const UINT width = cols * cellW;
    const UINT height = rows * cellH;
    // The same device flags AtlasEngine::_recreateBackend uses: single-threaded + BGRA (Direct2D), and
    // on hardware also PREVENT_INTERNAL_THREADING_OPTIMIZATIONS (cleared for WARP, which needs them).
    UINT flags = D3D11_CREATE_DEVICE_SINGLETHREADED | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (!warp)
    {
        flags |= D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS;
    }
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    CHECK(D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context));

    // An offscreen BGRA target the size of the terminal stands in for the swap chain's back buffer.
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    CHECK(device->CreateTexture2D(&td, nullptr, &texture));
    ComPtr<IDXGISurface> surface;
    CHECK(texture.As(&surface));

    ComPtr<ID2D1Factory1> d2d;
    CHECK(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), reinterpret_cast<void**>(d2d.GetAddressOf())));
    const D2D1_RENDER_TARGET_PROPERTIES props{ D2D1_RENDER_TARGET_TYPE_DEFAULT, { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED }, 96.f, 96.f };
    ComPtr<ID2D1RenderTarget> target;
    CHECK(d2d->CreateDxgiSurfaceRenderTarget(surface.Get(), &props, &target));
    target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    ComPtr<ID2D1DeviceContext> deviceContext;
    CHECK(target.As(&deviceContext));

    ComPtr<IDWriteFactory> dwrite;
    CHECK(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwrite.GetAddressOf())));
    ComPtr<IDWriteFontCollection> fonts;
    CHECK(dwrite->GetSystemFontCollection(&fonts));
    UINT32 familyIndex = 0;
    BOOL exists = FALSE;
    const wchar_t* faceName = L"Cascadia Mono";
    CHECK(fonts->FindFamilyName(faceName, &familyIndex, &exists));
    if (!exists)
    {
        faceName = L"Consolas";
        CHECK(fonts->FindFamilyName(faceName, &familyIndex, &exists));
    }
    ComPtr<IDWriteFontFamily> family;
    CHECK(fonts->GetFontFamily(familyIndex, &family));
    ComPtr<IDWriteFont> font;
    CHECK(family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font));
    ComPtr<IDWriteFontFace> face;
    CHECK(font->CreateFontFace(&face));

    const Screen screen = MakeScreen(face.Get(), cols, rows, fillPercent, runsPerRow);
    const std::vector<FLOAT> advances(cols, static_cast<FLOAT>(cellW));
    const std::vector<DWRITE_GLYPH_OFFSET> offsets(cols);

    // BackendD2D's background: a 1-cell-bordered color bitmap stretched over the target.
    const std::vector<UINT32> background(static_cast<size_t>(cols + 2) * (rows + 2), 0xFF0C0C0C);
    ComPtr<ID2D1Bitmap> backgroundBitmap;
    const D2D1_BITMAP_PROPERTIES bitmapProps{ { DXGI_FORMAT_R8G8B8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED }, 96.f, 96.f };
    CHECK(target->CreateBitmap(D2D1::SizeU(cols + 2, rows + 2), background.data(), (cols + 2) * 4, &bitmapProps, &backgroundBitmap));
    ComPtr<ID2D1BitmapBrush> backgroundBrush;
    CHECK(target->CreateBitmapBrush(backgroundBitmap.Get(), &backgroundBrush));
    backgroundBrush->SetInterpolationMode(D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    backgroundBrush->SetTransform(D2D1::Matrix3x2F(static_cast<FLOAT>(cellW), 0, 0, static_cast<FLOAT>(cellH), -static_cast<FLOAT>(cellW), -static_cast<FLOAT>(cellH)));
    const D2D1_COLOR_F palette[] = { { 0.8f, 0.8f, 0.8f, 1 }, { 0.4f, 0.7f, 1, 1 }, { 1, 0.6f, 0.3f, 1 }, { 0.5f, 0.9f, 0.5f, 1 }, { 0.9f, 0.4f, 0.9f, 1 }, { 1, 1, 1, 1 } };
    ComPtr<ID2D1SolidColorBrush> brushes[6];
    for (int i = 0; i < 6; ++i)
    {
        CHECK(target->CreateSolidColorBrush(palette[i], &brushes[i]));
    }

    D3D11_QUERY_DESC queryDesc{ D3D11_QUERY_EVENT, 0 };
    ComPtr<ID3D11Query> frameDone;
    CHECK(device->CreateQuery(&queryDesc, &frameDone));

    const FLOAT fontEm = 16.f; // 12 pt at 96 DPI
    const FLOAT baseline = cellH * 0.8f;
    const auto drawFrame = [&] {
        target->BeginDraw();
        deviceContext->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_COPY);
        target->FillRectangle(D2D1::RectF(0, 0, static_cast<FLOAT>(width), static_cast<FLOAT>(height)), backgroundBrush.Get());
        deviceContext->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
        for (int y = 0; y < rows; ++y)
        {
            FLOAT x = 0;
            int offset = 0;
            int colorIndex = y;
            for (const int len : screen.runLengths[y])
            {
                const DWRITE_GLYPH_RUN run{ face.Get(), fontEm, static_cast<UINT32>(len), &screen.glyphs[y][offset], &advances[offset], &offsets[offset], FALSE, 0 };
                target->DrawGlyphRun(D2D1::Point2F(x, y * cellH + baseline), &run, brushes[colorIndex++ % 6].Get(), DWRITE_MEASURING_MODE_NATURAL);
                x += static_cast<FLOAT>(len * cellW);
                offset += len;
            }
        }
        CHECK(target->EndDraw());
        // Wait until the device really finished this frame, so the CPU WARP spends rasterizing it
        // is inside the measurement instead of leaking into the next frame (or past the end).
        context->End(frameDone.Get());
        context->Flush();
        while (context->GetData(frameDone.Get(), nullptr, 0, 0) == S_FALSE)
        {
            SwitchToThread();
        }
    };

    for (int i = 0; i < 10; ++i)
    {
        drawFrame();
    }
    const double cpu0 = ProcessCpuMs();
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < frames; ++i)
    {
        drawFrame();
    }
    const double wallMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const double cpuMs = ProcessCpuMs() - cpu0;
    std::printf("%-5s %ux%u px (%dx%d cells, fill %d%%, %d runs/row, %ls): %.2f ms CPU/frame (%.2f ms wall/frame, %d frames)\n",
                label, width, height, cols, rows, fillPercent, runsPerRow, faceName, cpuMs / frames, wallMs / frames, frames);
}

// Prints the RAM <count> software-rendered terminals hold (see the file header's `mem` mode).
// Parameters: count — terminals to build (20+ drowns other processes' commit noise); width/height —
// one terminal's surface in pixels. Exits the process on any graphics failure (CHECK).
static void MemPerTerminal(const int count, const UINT width, const UINT height)
{
    struct Terminal
    {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        HANDLE surfaceHandle{};
        ComPtr<IDXGISwapChain1> swapChain;
    };
    ComPtr<ID2D1Factory1> d2d;
    CHECK(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), reinterpret_cast<void**>(d2d.GetAddressOf())));
    const D2D1_RENDER_TARGET_PROPERTIES props{ D2D1_RENDER_TARGET_TYPE_DEFAULT, { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED }, 96.f, 96.f };
    std::vector<Terminal> terminals(count);
    const double privateBefore = PrivateMb();
    const double commitBefore = CommitMb();
    for (auto& t : terminals)
    {
        CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_SINGLETHREADED | D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &t.device, nullptr, &t.context));
        ComPtr<IDXGIDevice> dxgiDevice;
        CHECK(t.device.As(&dxgiDevice));
        ComPtr<IDXGIAdapter> adapter;
        CHECK(dxgiDevice->GetAdapter(&adapter));
        ComPtr<IDXGIFactoryMedia> factory;
        CHECK(adapter->GetParent(__uuidof(IDXGIFactoryMedia), reinterpret_cast<void**>(factory.GetAddressOf())));
        // AtlasEngine::_createSwapChain's recipe: a DirectComposition surface handle (what the XAML
        // SwapChainPanel consumes) and a 3-buffer FLIP_SEQUENTIAL chain with a frame-latency waitable.
        CHECK(DCompositionCreateSurfaceHandle(0x0003L /* COMPOSITIONSURFACE_ALL_ACCESS */, nullptr, &t.surfaceHandle));
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
        CHECK(factory->CreateSwapChainForCompositionSurfaceHandle(t.device.Get(), t.surfaceHandle, &desc, nullptr, &t.swapChain));
        // Write each of the three buffers once (draw, present, rotate): an untouched buffer may not
        // be committed yet, which would under-report what a painting terminal holds.
        for (int frame = 0; frame < 3; ++frame)
        {
            ComPtr<IDXGISurface> backBuffer;
            CHECK(t.swapChain->GetBuffer(0, __uuidof(IDXGISurface), reinterpret_cast<void**>(backBuffer.GetAddressOf())));
            ComPtr<ID2D1RenderTarget> target;
            CHECK(d2d->CreateDxgiSurfaceRenderTarget(backBuffer.Get(), &props, &target));
            target->BeginDraw();
            target->Clear(D2D1::ColorF(0.05f, 0.05f, 0.05f + frame * 0.01f));
            CHECK(target->EndDraw());
            t.swapChain->Present(0, 0);
        }
    }
    const double privateAfter = PrivateMb();
    const double commitAfter = CommitMb();
    std::printf("RAM   %d WARP terminals at %ux%u px: +%.1f MB private, +%.1f MB system commit (%.1f MB commit per terminal)\n",
                count, width, height, privateAfter - privateBefore, commitAfter - commitBefore, (commitAfter - commitBefore) / count);
    for (auto& t : terminals)
    {
        CloseHandle(t.surfaceHandle);
    }
}

// Entry point. Exit codes: 0 measured; 1 a graphics call failed (CHECK); 2 usage error.
int main(int argc, char** argv)
{
    if (argc == 5 && std::string(argv[1]) == "mem")
    {
        MemPerTerminal(atoi(argv[2]), static_cast<UINT>(atoi(argv[3])), static_cast<UINT>(atoi(argv[4])));
        return 0;
    }
    if (argc < 8)
    {
        std::fprintf(stderr,
                     "usage: warpbench <cols> <rows> <cellW> <cellH> <fillPercent> <colorRunsPerRow> <frames> [hw|warp|both]\n"
                     "       warpbench mem <count> <widthPx> <heightPx>\n"
                     "e.g.   warpbench 178 51 9 19 60 4 300 both   (a busy Claude screen in a 1920x1032 window at 96 DPI)\n");
        return 2;
    }
    const int cols = atoi(argv[1]), rows = atoi(argv[2]), cellW = atoi(argv[3]), cellH = atoi(argv[4]);
    const int fill = atoi(argv[5]), runs = atoi(argv[6]), frames = atoi(argv[7]);
    const std::string which = argc > 8 ? argv[8] : "both";
    if (cols <= 0 || rows <= 0 || cellW <= 0 || cellH <= 0 || fill < 0 || fill > 100 || runs <= 0 || frames <= 0)
    {
        std::fprintf(stderr, "warpbench: every size/count must be positive and fillPercent 0..100\n");
        return 2;
    }
    if (which == "hw" || which == "both")
    {
        Bench("GPU", false, cols, rows, cellW, cellH, fill, runs, frames);
    }
    if (which == "warp" || which == "both")
    {
        Bench("WARP", true, cols, rows, cellW, cellH, fill, runs, frames);
    }
    return 0;
}
