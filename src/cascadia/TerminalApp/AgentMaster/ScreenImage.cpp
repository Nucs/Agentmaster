// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster - ScreenImage: the drawing half (ScreenImage.h says what it draws, how it imitates the terminal, and why the
// screen is drawn again from the text buffer instead of being captured). Direct2D on a WIC bitmap, DirectWrite for the
// text, WIC for the PNG. Plain C++ + COM (no WinRT, no PCH).

#include "ScreenImage.h"

#include <windows.h>
#include <d2d1.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <unordered_map>
#include <vector>

#include "ClaudeSpawn.h" // LogSwallowedException (Rule #18)

#pragma comment(lib, "d2d1.lib") // D2D1CreateFactory
#pragma comment(lib, "dwrite.lib") // DWriteCreateFactory
#pragma comment(lib, "windowscodecs.lib") // the WIC GUIDs (CLSID_WICImagingFactory, the PNG container, the pixel formats)
#pragma comment(lib, "ole32.lib") // CoInitializeEx, CoCreateInstance, CreateStreamOnHGlobal

using Microsoft::WRL::ComPtr;

namespace Agentmaster
{
    namespace
    {
        /// <summary>A failed call as the result's error text: "&lt;what&gt; failed (0x8899000C)".</summary>
        /// <param name="what">The call that failed.</param>
        /// <param name="hr">Its HRESULT.</param>
        /// <returns>The text.</returns>
        /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
        std::wstring Failed(const wchar_t* what, HRESULT hr)
        {
            wchar_t code[16]{};
            swprintf_s(code, L"0x%08X", static_cast<unsigned int>(hr));
            return std::wstring{ what } + L" failed (" + code + L")";
        }

        /// <summary>
        /// COM on the calling thread for one call. Initialized multithreaded; uninitialized at the end only when THIS call
        /// initialized it (S_OK, or S_FALSE when it was initialized the same way already - both need the balancing call). A
        /// thread that is single-threaded already (RPC_E_CHANGED_MODE) keeps its model and is left alone: WIC and Direct2D
        /// work in either, and uninitializing a model this call did not set would pull COM out from under its owner.
        /// </summary>
        struct ComForThisCall
        {
            HRESULT hr;
            ComForThisCall() noexcept :
                hr(::CoInitializeEx(nullptr, COINIT_MULTITHREADED))
            {
            }
            ~ComForThisCall()
            {
                if (SUCCEEDED(hr))
                {
                    ::CoUninitialize();
                }
            }
            ComForThisCall(const ComForThisCall&) = delete;
            ComForThisCall& operator=(const ComForThisCall&) = delete;
        };

        /// <summary>The monospace families tried after the snapshot's own font, in this order.</summary>
        constexpr const wchar_t* kFallbackFamilies[] = { L"Cascadia Mono", L"Cascadia Code", L"Consolas", L"Lucida Console", L"Courier New" };

        /// <summary>The font a screen is drawn in: its family and its regular face's proportions, each as a fraction of the em.</summary>
        struct GridFont
        {
            std::wstring family;
            double advanceEm{ 0 }; // a narrow glyph's advance ('0')
            double lineHeightEm{ 0 }; // ascent + descent + line gap
            double ascentEm{ 0 }; // the baseline's depth below the line's top
            double underlineEm{ 0 }; // the underline's position, above the baseline when positive (it is normally negative)
            double underlineThicknessEm{ 0 };
            double strikeEm{ 0 }; // the strikethrough's position above the baseline
            double strikeThicknessEm{ 0 };
        };

        /// <summary>
        /// Finds the first family the system has - the snapshot's own font, then kFallbackFamilies - whose regular face has
        /// a usable em and a '0' with an advance, and reads its proportions. A terminal font that is only bundled with the app
        /// (not installed for the system) is not in the system collection: the fallbacks draw it then.
        /// </summary>
        /// <param name="dw">The DirectWrite factory.</param>
        /// <param name="wanted">The snapshot's font family ("" = none).</param>
        /// <param name="out">The font found.</param>
        /// <param name="error">Why none was found.</param>
        /// <returns>True when a font was found.</returns>
        /// <exception cref="std::bad_alloc">If a name cannot be copied.</exception>
        bool PickGridFont(IDWriteFactory* dw, const std::wstring& wanted, GridFont& out, std::wstring& error)
        {
            ComPtr<IDWriteFontCollection> collection;
            const HRESULT hr = dw->GetSystemFontCollection(&collection, FALSE);
            if (FAILED(hr))
            {
                error = Failed(L"GetSystemFontCollection", hr);
                return false;
            }
            std::vector<std::wstring> names;
            if (!wanted.empty())
            {
                names.push_back(wanted);
            }
            for (const wchar_t* fallback : kFallbackFamilies)
            {
                names.emplace_back(fallback);
            }
            for (const auto& name : names)
            {
                UINT32 index = 0;
                BOOL exists = FALSE;
                if (FAILED(collection->FindFamilyName(name.c_str(), &index, &exists)) || !exists)
                {
                    continue;
                }
                ComPtr<IDWriteFontFamily> family;
                ComPtr<IDWriteFont> font;
                ComPtr<IDWriteFontFace> face;
                if (FAILED(collection->GetFontFamily(index, &family)) ||
                    FAILED(family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font)) ||
                    FAILED(font->CreateFontFace(&face)))
                {
                    continue;
                }
                DWRITE_FONT_METRICS metrics{};
                face->GetMetrics(&metrics);
                if (metrics.designUnitsPerEm == 0)
                {
                    continue;
                }
                const UINT32 zero = L'0';
                UINT16 glyph = 0;
                DWRITE_GLYPH_METRICS glyphMetrics{};
                if (FAILED(face->GetGlyphIndices(&zero, 1, &glyph)) || glyph == 0 || FAILED(face->GetDesignGlyphMetrics(&glyph, 1, &glyphMetrics, FALSE)) || glyphMetrics.advanceWidth == 0)
                {
                    continue; // not a font a grid can be measured with
                }
                const double upem = static_cast<double>(metrics.designUnitsPerEm);
                out.family = name;
                out.advanceEm = static_cast<double>(glyphMetrics.advanceWidth) / upem;
                out.lineHeightEm = (static_cast<double>(metrics.ascent) + metrics.descent + metrics.lineGap) / upem;
                out.ascentEm = static_cast<double>(metrics.ascent) / upem;
                out.underlineEm = static_cast<double>(metrics.underlinePosition) / upem;
                out.underlineThicknessEm = static_cast<double>(metrics.underlineThickness) / upem;
                out.strikeEm = static_cast<double>(metrics.strikethroughPosition) / upem;
                out.strikeThicknessEm = static_cast<double>(metrics.strikethroughThickness) / upem;
                return true;
            }
            error = L"no monospace font was found (the screen's own, Cascadia Mono, Cascadia Code, Consolas, Lucida Console, Courier New)";
            return false;
        }

        /// <summary>
        /// Encodes the drawn bitmap as PNG bytes: converted to 24-bit BGR first (the picture is opaque, and an alpha channel
        /// would only make the file bigger), written into a memory stream, and copied out of it.
        /// </summary>
        /// <param name="wic">The WIC factory.</param>
        /// <param name="bitmap">The drawn bitmap (32bppPBGRA, fully opaque).</param>
        /// <param name="width">Its width in pixels.</param>
        /// <param name="height">Its height in pixels.</param>
        /// <param name="png">The PNG bytes.</param>
        /// <param name="error">What failed.</param>
        /// <returns>True when `png` holds the file.</returns>
        /// <exception cref="std::bad_alloc">If the bytes cannot be copied.</exception>
        bool EncodePng(IWICImagingFactory* wic, IWICBitmap* bitmap, UINT width, UINT height, std::string& png, std::wstring& error)
        {
            ComPtr<IStream> stream;
            HRESULT hr = ::CreateStreamOnHGlobal(nullptr, TRUE, &stream);
            if (FAILED(hr))
            {
                error = Failed(L"CreateStreamOnHGlobal", hr);
                return false;
            }
            ComPtr<IWICBitmapEncoder> encoder;
            hr = wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
            if (SUCCEEDED(hr))
            {
                hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
            }
            ComPtr<IWICBitmapFrameEncode> frame;
            ComPtr<IPropertyBag2> frameProperties;
            if (SUCCEEDED(hr))
            {
                hr = encoder->CreateNewFrame(&frame, &frameProperties);
            }
            if (SUCCEEDED(hr))
            {
                hr = frame->Initialize(frameProperties.Get());
            }
            if (SUCCEEDED(hr))
            {
                hr = frame->SetSize(width, height);
            }
            // The encoder may answer with the nearest format it supports (24bppBGR is one of PNG's): the converter follows it.
            WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
            if (SUCCEEDED(hr))
            {
                hr = frame->SetPixelFormat(&format);
            }
            ComPtr<IWICFormatConverter> converter;
            if (SUCCEEDED(hr))
            {
                hr = wic->CreateFormatConverter(&converter);
            }
            if (SUCCEEDED(hr))
            {
                hr = converter->Initialize(bitmap, format, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
            }
            if (SUCCEEDED(hr))
            {
                hr = frame->WriteSource(converter.Get(), nullptr);
            }
            if (SUCCEEDED(hr))
            {
                hr = frame->Commit();
            }
            if (SUCCEEDED(hr))
            {
                hr = encoder->Commit();
            }
            if (FAILED(hr))
            {
                error = Failed(L"the PNG encoding", hr);
                return false;
            }
            // The stream's size, not the HGLOBAL's: GlobalSize can be rounded up past the bytes written.
            STATSTG stat{};
            hr = stream->Stat(&stat, STATFLAG_NONAME);
            HGLOBAL memory = nullptr;
            if (SUCCEEDED(hr))
            {
                hr = ::GetHGlobalFromStream(stream.Get(), &memory);
            }
            if (FAILED(hr) || !memory || stat.cbSize.HighPart != 0)
            {
                error = Failed(L"reading the PNG stream", FAILED(hr) ? hr : E_UNEXPECTED);
                return false;
            }
            const void* bytes = ::GlobalLock(memory);
            if (!bytes)
            {
                error = Failed(L"GlobalLock", HRESULT_FROM_WIN32(::GetLastError()));
                return false;
            }
            try
            {
                png.assign(static_cast<const char*>(bytes), static_cast<size_t>(stat.cbSize.LowPart));
            }
            catch (...)
            {
                ::GlobalUnlock(memory); // the lock is released on every path; the bad_alloc goes on to the caller's net
                throw;
            }
            ::GlobalUnlock(memory);
            return true;
        }
    }

    ScreenImage RenderScreenSnapshotPng(const ScreenSnapshot& snapshot, const ScreenImageOptions& options) noexcept
    {
        ScreenImage out;
        try
        {
            if (snapshot.cols <= 0 || snapshot.rows <= 0 || snapshot.cols > kScreenSnapshotMaxCols || snapshot.rows > kScreenSnapshotMaxRows)
            {
                out.error = L"the screen has no usable size";
                return out;
            }
            const ComForThisCall com;
            if (FAILED(com.hr) && com.hr != RPC_E_CHANGED_MODE)
            {
                out.error = Failed(L"CoInitializeEx", com.hr);
                return out;
            }
            ComPtr<IDWriteFactory> dw;
            HRESULT hr = ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dw.GetAddressOf()));
            if (FAILED(hr))
            {
                out.error = Failed(L"DWriteCreateFactory", hr);
                return out;
            }
            GridFont font;
            if (!PickGridFont(dw.Get(), snapshot.fontFace, font, out.error))
            {
                return out;
            }
            out.fontFamily = font.family;
            const ScreenImageGrid grid = ComputeScreenImageGrid(snapshot.cols, snapshot.rows, font.advanceEm, font.lineHeightEm, options);
            if (grid.cellWidthPx <= 0 || grid.widthPx <= 0 || grid.heightPx <= 0)
            {
                out.error = L"the font gives no usable cell";
                return out;
            }
            out.grid = grid;
            const float cellW = static_cast<float>(grid.cellWidthPx);
            const float cellH = static_cast<float>(grid.cellHeightPx);
            const float lineH = grid.lineHeightPx;
            const float fontPx = grid.fontPx;
            const float baseline = static_cast<float>(font.ascentEm * fontPx);
            const float advancePx = static_cast<float>(font.advanceEm * fontPx); // == cellW, by ComputeScreenImageGrid's choice of fontPx
            const float pad = static_cast<float>((std::max)(0, options.paddingPx)); // the same clamp ComputeScreenImageGrid applies
            // A line's top within its cell: the font's line centered in the cell (the cell is under a pixel taller).
            const float lineInset = (cellH - lineH) / 2.0f;

            // The four faces a cell can ask for, indexed by bold (1) | italic (2). Each draws on the same uniform line, so a
            // glyph from a fallback font sits on the grid's baseline like the rest and the rows stay level.
            ComPtr<IDWriteTextFormat> formats[4];
            for (int i = 0; i < 4; ++i)
            {
                hr = dw->CreateTextFormat(font.family.c_str(), nullptr, (i & 1) ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL, (i & 2) ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fontPx, L"en-us", &formats[i]);
                if (FAILED(hr))
                {
                    out.error = Failed(L"CreateTextFormat", hr);
                    return out;
                }
                formats[i]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                formats[i]->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, lineH, baseline);
            }

            ComPtr<IWICImagingFactory> wic;
            hr = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
            if (FAILED(hr))
            {
                out.error = Failed(L"CoCreateInstance(WICImagingFactory)", hr);
                return out;
            }
            ComPtr<IWICBitmap> bitmap;
            hr = wic->CreateBitmap(static_cast<UINT>(grid.widthPx), static_cast<UINT>(grid.heightPx), GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap);
            if (FAILED(hr))
            {
                out.error = Failed(L"CreateBitmap", hr);
                return out;
            }
            ComPtr<ID2D1Factory> d2d;
            hr = ::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf()); // single-threaded: this call's thread only
            if (FAILED(hr))
            {
                out.error = Failed(L"D2D1CreateFactory", hr);
                return out;
            }
            const D2D1_RENDER_TARGET_PROPERTIES targetProperties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
            ComPtr<ID2D1RenderTarget> target;
            hr = d2d->CreateWicBitmapRenderTarget(bitmap.Get(), &targetProperties, &target);
            if (FAILED(hr))
            {
                out.error = Failed(L"CreateWicBitmapRenderTarget", hr);
                return out;
            }
            // Grayscale text: the picture is zoomed and recompressed by the messenger, where ClearType's color fringes smear.
            target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            // The backgrounds and the lines sit on whole pixels: aliased edges keep neighboring cells from bleeding a seam.
            target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
            // Color emoji need a device context (Direct2D 1.1, Windows 8.1+); without one they draw in the cell's color.
            ComPtr<ID2D1DeviceContext> deviceContext;
            const D2D1_DRAW_TEXT_OPTIONS textOptions = SUCCEEDED(target.As(&deviceContext)) ? D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT : D2D1_DRAW_TEXT_OPTIONS_NONE;

            // One brush per color the screen uses (a terminal screen holds a handful). A brush that could not be made is
            // remembered as null and its cells are skipped: a picture with a missing color beats no picture.
            std::unordered_map<uint32_t, ComPtr<ID2D1SolidColorBrush>> brushes;
            const auto brushFor = [&](uint32_t rgb) -> ID2D1SolidColorBrush* {
                const uint32_t key = rgb & 0xFFFFFFu;
                if (const auto it = brushes.find(key); it != brushes.end())
                {
                    return it->second.Get();
                }
                ComPtr<ID2D1SolidColorBrush> brush;
                if (FAILED(target->CreateSolidColorBrush(D2D1::ColorF(key), &brush)))
                {
                    brush.Reset();
                }
                return brushes.emplace(key, std::move(brush)).first->second.Get();
            };
            const uint32_t defaultBg = snapshot.defaultBg & 0xFFFFFFu;
            const float underlineOffset = baseline - static_cast<float>(font.underlineEm * fontPx); // positions are measured upward
            const float underlineThickness = (std::max)(1.0f, std::round(static_cast<float>(font.underlineThicknessEm * fontPx)));
            const float strikeOffset = baseline - static_cast<float>(font.strikeEm * fontPx);
            const float strikeThickness = (std::max)(1.0f, std::round(static_cast<float>(font.strikeThicknessEm * fontPx)));

            target->BeginDraw();
            target->Clear(D2D1::ColorF(defaultBg));
            const size_t rowCount = (std::min)(static_cast<size_t>(snapshot.rows), snapshot.lines.size());
            for (size_t y = 0; y < rowCount; ++y)
            {
                const auto& line = snapshot.lines[y];
                const float top = pad + static_cast<float>(y) * cellH;
                // The backgrounds first, one rectangle per run of one color, so no glyph is painted over by a later cell's
                // background (an italic overhang, a fallback glyph wider than its cell).
                int column = 0;
                size_t i = 0;
                while (i < line.size())
                {
                    const uint32_t bg = line[i].bg & 0xFFFFFFu;
                    const int start = column;
                    while (i < line.size() && (line[i].bg & 0xFFFFFFu) == bg)
                    {
                        column += line[i].width == 2 ? 2 : 1;
                        ++i;
                    }
                    if (bg != defaultBg)
                    {
                        if (auto* brush = brushFor(bg))
                        {
                            target->FillRectangle(D2D1::RectF(pad + static_cast<float>(start) * cellW, top, pad + static_cast<float>(column) * cellW, top + cellH), brush);
                        }
                    }
                }
                // Then each cell's glyph and its lines.
                column = 0;
                for (const auto& cell : line)
                {
                    const int width = cell.width == 2 ? 2 : 1;
                    const float left = pad + static_cast<float>(column) * cellW;
                    column += width;
                    auto* fg = brushFor(cell.fg);
                    if (!fg)
                    {
                        continue;
                    }
                    if (!cell.text.empty() && cell.text != L" ")
                    {
                        IDWriteTextFormat* format = formats[((cell.flags & kScreenCellBold) ? 1 : 0) | ((cell.flags & kScreenCellItalic) ? 2 : 0)].Get();
                        const UINT32 length = static_cast<UINT32>(cell.text.size());
                        if (width == 1 && IsCellStretchGlyph(cell.text))
                        {
                            // A frame or rule piece: the font's line box (one advance wide, one line tall) mapped onto the cell
                            // exactly, so its strokes reach the cell's edges and join the next cell's, as the terminal's own
                            // built-in box glyphs do. The horizontal scale is 1 by construction; the vertical one is at most one
                            // pixel's worth of the line.
                            target->SetTransform(D2D1::Matrix3x2F::Scale(cellW / advancePx, cellH / lineH) * D2D1::Matrix3x2F::Translation(left, top));
                            target->DrawText(cell.text.c_str(), length, format, D2D1::RectF(0.0f, 0.0f, 2.0f * advancePx, lineH), fg, textOptions);
                            target->SetTransform(D2D1::Matrix3x2F::Identity());
                        }
                        else
                        {
                            // Any other glyph alone in its cell (or its two cells): the layout box is a cell wider than the glyph's
                            // share so a slightly wider fallback glyph is never cut, and no-wrap keeps it on the line.
                            const float lineTop = top + lineInset;
                            target->DrawText(cell.text.c_str(), length, format, D2D1::RectF(left, lineTop, left + static_cast<float>(width + 1) * cellW, lineTop + lineH), fg, textOptions);
                        }
                    }
                    if (cell.flags & kScreenCellUnderline)
                    {
                        const float lineY = std::round(top + lineInset + underlineOffset);
                        target->FillRectangle(D2D1::RectF(left, lineY, left + static_cast<float>(width) * cellW, lineY + underlineThickness), fg);
                    }
                    if (cell.flags & kScreenCellStrikethrough)
                    {
                        const float lineY = std::round(top + lineInset + strikeOffset);
                        target->FillRectangle(D2D1::RectF(left, lineY, left + static_cast<float>(width) * cellW, lineY + strikeThickness), fg);
                    }
                }
            }
            hr = target->EndDraw();
            if (FAILED(hr))
            {
                out.error = Failed(L"EndDraw", hr);
                return out;
            }
            if (!EncodePng(wic.Get(), bitmap.Get(), static_cast<UINT>(grid.widthPx), static_cast<UINT>(grid.heightPx), out.png, out.error))
            {
                out.png.clear();
                return out;
            }
            return out;
        }
        catch (...)
        {
            // Rule #18: the throw site goes to hooks.log; the caller gets a reason instead of a picture.
            LogSwallowedException(L"RenderScreenSnapshotPng");
            ScreenImage failed;
            failed.error = L"an exception while drawing the screen";
            return failed;
        }
    }
}
