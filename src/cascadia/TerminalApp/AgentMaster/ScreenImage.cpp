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

    // ===== TALL pictures (MESSAGING.md section 21.39): ScreenImage.h says what they are and why they are drawn in strips =====

    namespace
    {
        /// <summary>
        /// The ScreenImageOptions that make ComputeScreenImageGrid pick exactly the tall options' cell: a width budget of exactly
        /// `cols` such cells (plus the padding) and a cell range of that one width. The grid arithmetic then stays the screen's,
        /// so a tall picture's font, cell height and baseline follow the very same rules as a Screen Image's.
        /// </summary>
        /// <param name="cols">The sheet's columns.</param>
        /// <param name="options">The tall options (a cell below 1 px counts as 1, a negative padding as 0).</param>
        /// <returns>The options.</returns>
        ScreenImageOptions FixedCellOptions(int cols, const TallScreenImageOptions& options) noexcept
        {
            ScreenImageOptions fixed;
            const int cell = options.cellWidthPx < 1 ? 1 : options.cellWidthPx;
            fixed.paddingPx = options.paddingPx < 0 ? 0 : options.paddingPx;
            fixed.minCellWidthPx = cell;
            fixed.maxCellWidthPx = cell;
            fixed.maxWidthPx = cols * cell + 2 * fixed.paddingPx;
            return fixed;
        }

        /// <summary>
        /// The character a cell contributes to a batched ASCII run, or 0 when it must be drawn alone. A run holds only cells of
        /// ONE narrow column whose text is one printable ASCII unit or a blank: every font a sheet is drawn in is monospace and
        /// has all of them, so their advances are exactly the cell (ComputeScreenImageGrid sizes the font that way) and no
        /// glyph of the run comes from a fallback font with another advance. A wide glyph, a box-drawing piece (stretched to its
        /// cell), a symbol or an emoji, or a cluster of several code units is drawn alone.
        /// </summary>
        /// <param name="cell">The cell.</param>
        /// <returns>The character (' ' for a blank cell), or 0.</returns>
        wchar_t AsciiRunChar(const ScreenCell& cell) noexcept
        {
            if (cell.width == 2)
            {
                return 0;
            }
            if (cell.text.empty())
            {
                return L' ';
            }
            if (cell.text.size() == 1 && cell.text[0] >= 0x20 && cell.text[0] <= 0x7E)
            {
                return cell.text[0];
            }
            return 0;
        }

        /// <summary>One brush per color a sheet uses, made on first use. A brush that could not be made is remembered as null, and the cells in its color are skipped (a picture with a missing color beats no picture).</summary>
        struct BrushCache
        {
            ID2D1RenderTarget* target{ nullptr }; // the target the brushes belong to (a brush is only valid on its own target)
            std::unordered_map<uint32_t, ComPtr<ID2D1SolidColorBrush>> brushes; // by 0xRRGGBB; a null entry = could not be made

            /// <summary>The brush of a color.</summary>
            /// <param name="rgb">The color as 0xRRGGBB (higher bits are ignored).</param>
            /// <returns>The brush, or null when it could not be made.</returns>
            /// <exception cref="std::bad_alloc">If the cache cannot grow.</exception>
            ID2D1SolidColorBrush* For(uint32_t rgb)
            {
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
            }
        };

        /// <summary>
        /// Draws the rows of a tall sheet into the current strip, at strip-local coordinates. Everything a row draws is the
        /// screen renderer's look (backgrounds as merged runs, then glyphs, box-drawing pieces stretched to their cell, then the
        /// underline and the strikethrough); only runs of plain ASCII in one color and one face are drawn with ONE DrawText
        /// each (AsciiRunChar says why that keeps them on the grid). The values are the grid's, computed once per sheet.
        /// </summary>
        struct TallPainter
        {
            ID2D1RenderTarget* target{ nullptr }; // the strip's target (borrowed: the caller owns it for the whole drawing)
            IDWriteTextFormat* formats[4]{}; // regular, bold, italic, bold italic: index bold (1) | italic (2)
            BrushCache* brushes{ nullptr }; // the target's brushes (borrowed)
            float cellW{ 0 }; // a column's width in pixels (whole: ComputeScreenImageGrid)
            float cellH{ 0 }; // a row's height in pixels (whole)
            float lineH{ 0 }; // the font's own line height (the cell is up to a pixel taller)
            float lineInset{ 0 }; // the line's top within its cell: the line centered in the cell
            float advancePx{ 0 }; // the font's advance at its size: == cellW by the grid's choice of font size
            float pad{ 0 };
            float underlineOffset{ 0 }; // below a line's top
            float underlineThickness{ 1 };
            float strikeOffset{ 0 };
            float strikeThickness{ 1 };
            uint32_t defaultBg{ 0 };
            D2D1_DRAW_TEXT_OPTIONS textOptions{ D2D1_DRAW_TEXT_OPTIONS_NONE };

            /// <summary>The text format for a cell's bold and italic bits.</summary>
            /// <param name="flags">The cell's kScreenCell* bits.</param>
            /// <returns>The format (never null once the painter is set up).</returns>
            IDWriteTextFormat* FormatFor(uint8_t flags) const noexcept
            {
                return formats[((flags & kScreenCellBold) ? 1 : 0) | ((flags & kScreenCellItalic) ? 2 : 0)];
            }

            /// <summary>The underline and the strikethrough of one cell (or of a wide cell's two columns), in its foreground.</summary>
            /// <param name="cell">The cell.</param>
            /// <param name="left">The cell's left edge (strip-local).</param>
            /// <param name="top">The cell's top edge (strip-local).</param>
            /// <param name="width">The columns it covers (1 or 2).</param>
            /// <exception cref="std::bad_alloc">If a brush cannot be cached.</exception>
            void Lines(const ScreenCell& cell, float left, float top, int width)
            {
                if ((cell.flags & (kScreenCellUnderline | kScreenCellStrikethrough)) == 0)
                {
                    return;
                }
                auto* fg = brushes->For(cell.fg);
                if (!fg)
                {
                    return;
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

            /// <summary>Draws one sheet row whose cell top is `top` (strip-local; it may lie above or below the strip, which clips).</summary>
            /// <param name="line">The row's cells, left to right (trailing blanks may be absent).</param>
            /// <param name="top">The row's top edge in the strip.</param>
            /// <exception cref="std::bad_alloc">If the run text or a brush cannot be allocated.</exception>
            void Row(const std::vector<ScreenCell>& line, float top)
            {
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
                        if (auto* brush = brushes->For(bg))
                        {
                            target->FillRectangle(D2D1::RectF(pad + static_cast<float>(start) * cellW, top, pad + static_cast<float>(column) * cellW, top + cellH), brush);
                        }
                    }
                }
                // Then the glyphs and their lines.
                std::wstring run;
                column = 0;
                i = 0;
                while (i < line.size())
                {
                    const ScreenCell& cell = line[i];
                    const float left = pad + static_cast<float>(column) * cellW;
                    if (AsciiRunChar(cell) != 0)
                    {
                        // A run: the cells that follow in the same color and face. Its leading and trailing blanks draw nothing,
                        // so they are left out of the call (the first glyph then sits exactly on its own cell).
                        const uint8_t face = cell.flags & (kScreenCellBold | kScreenCellItalic);
                        const uint32_t color = cell.fg & 0xFFFFFFu;
                        run.clear();
                        size_t j = i;
                        while (j < line.size())
                        {
                            const wchar_t c = AsciiRunChar(line[j]);
                            if (c == 0 || (line[j].fg & 0xFFFFFFu) != color || (line[j].flags & (kScreenCellBold | kScreenCellItalic)) != face)
                            {
                                break;
                            }
                            run.push_back(c);
                            ++j;
                        }
                        const size_t lead = run.find_first_not_of(L' ');
                        if (lead != std::wstring::npos)
                        {
                            const size_t ink = run.find_last_not_of(L' ') + 1 - lead;
                            if (auto* fg = brushes->For(color))
                            {
                                const float runLeft = left + static_cast<float>(lead) * cellW;
                                const float lineTop = top + lineInset;
                                // One cell of slack on the right, as a lone glyph gets: no glyph is ever cut at the box's edge.
                                target->DrawText(run.c_str() + lead, static_cast<UINT32>(ink), FormatFor(cell.flags), D2D1::RectF(runLeft, lineTop, runLeft + static_cast<float>(ink + 1) * cellW, lineTop + lineH), fg, textOptions);
                            }
                        }
                        for (size_t k = i; k < j; ++k)
                        {
                            Lines(line[k], left + static_cast<float>(k - i) * cellW, top, 1);
                        }
                        column += static_cast<int>(j - i);
                        i = j;
                        continue;
                    }
                    const int width = cell.width == 2 ? 2 : 1;
                    column += width;
                    ++i;
                    auto* fg = brushes->For(cell.fg);
                    if (!fg)
                    {
                        continue;
                    }
                    if (!cell.text.empty() && cell.text != L" ")
                    {
                        const UINT32 length = static_cast<UINT32>(cell.text.size());
                        if (width == 1 && IsCellStretchGlyph(cell.text))
                        {
                            // A frame or rule piece: the font's line box mapped onto the cell exactly (RenderScreenSnapshotPng).
                            target->SetTransform(D2D1::Matrix3x2F::Scale(cellW / advancePx, cellH / lineH) * D2D1::Matrix3x2F::Translation(left, top));
                            target->DrawText(cell.text.c_str(), length, FormatFor(cell.flags), D2D1::RectF(0.0f, 0.0f, 2.0f * advancePx, lineH), fg, textOptions);
                            target->SetTransform(D2D1::Matrix3x2F::Identity());
                        }
                        else
                        {
                            // Any other glyph alone in its cell (or its two), with a cell of slack so a wider fallback glyph is not cut.
                            const float lineTop = top + lineInset;
                            target->DrawText(cell.text.c_str(), length, FormatFor(cell.flags), D2D1::RectF(left, lineTop, left + static_cast<float>(width + 1) * cellW, lineTop + lineH), fg, textOptions);
                        }
                    }
                    Lines(cell, left, top, width);
                }
            }
        };

        /// <summary>
        /// Copies the bytes written into an HGLOBAL-backed stream out: the stream's size, not the HGLOBAL's (GlobalSize can be
        /// rounded up past the bytes written).
        /// </summary>
        /// <param name="stream">A stream made by CreateStreamOnHGlobal.</param>
        /// <param name="bytes">The bytes.</param>
        /// <param name="error">What failed.</param>
        /// <returns>True when `bytes` holds them.</returns>
        /// <exception cref="std::bad_alloc">If the bytes cannot be copied (the lock is released first).</exception>
        bool CopyStreamBytes(IStream* stream, std::string& bytes, std::wstring& error)
        {
            STATSTG stat{};
            HRESULT hr = stream->Stat(&stat, STATFLAG_NONAME);
            HGLOBAL memory = nullptr;
            if (SUCCEEDED(hr))
            {
                hr = ::GetHGlobalFromStream(stream, &memory);
            }
            if (FAILED(hr) || !memory || stat.cbSize.HighPart != 0)
            {
                error = Failed(L"reading the PNG stream", FAILED(hr) ? hr : E_UNEXPECTED);
                return false;
            }
            const void* data = ::GlobalLock(memory);
            if (!data)
            {
                error = Failed(L"GlobalLock", HRESULT_FROM_WIN32(::GetLastError()));
                return false;
            }
            try
            {
                bytes.assign(static_cast<const char*>(data), static_cast<size_t>(stat.cbSize.LowPart));
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

    ScreenImageGrid MeasureTallScreenImageGrid(const std::wstring& fontFace, int cols, const TallScreenImageOptions& options, std::wstring& fontFamily, std::wstring& error) noexcept
    {
        try
        {
            fontFamily.clear();
            error.clear();
            if (cols <= 0 || cols > kScreenSnapshotMaxCols)
            {
                error = L"the sheet has no usable width";
                return {};
            }
            // DirectWrite needs no COM, so nothing is initialized here: a measure is cheap enough for any worker thread.
            ComPtr<IDWriteFactory> dw;
            const HRESULT hr = ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dw.GetAddressOf()));
            if (FAILED(hr))
            {
                error = Failed(L"DWriteCreateFactory", hr);
                return {};
            }
            GridFont font;
            if (!PickGridFont(dw.Get(), fontFace, font, error))
            {
                return {};
            }
            const ScreenImageGrid grid = ComputeScreenImageGrid(cols, 1, font.advanceEm, font.lineHeightEm, FixedCellOptions(cols, options));
            if (grid.cellWidthPx <= 0 || grid.cellHeightPx <= 0)
            {
                error = L"the font gives no usable cell";
                return {};
            }
            fontFamily = font.family;
            return grid;
        }
        catch (...)
        {
            // Rule #18: logged with its throw site; the caller gets a reason (std::bad_alloc while copying a name is the only throw).
            LogSwallowedException(L"MeasureTallScreenImageGrid");
            error = L"an exception while measuring the font";
            return {};
        }
    }

    TallScreenImage RenderTallScreenSnapshotPngs(const ScreenSnapshot& sheet,
                                                 const std::vector<uint8_t>& cuts,
                                                 size_t maxParts,
                                                 const TallScreenImageOptions& options,
                                                 const std::function<void(TallImagePart&& part, size_t index, size_t count)>& onPart) noexcept
    {
        TallScreenImage out;
        try
        {
            if (sheet.cols <= 0 || sheet.rows <= 0 || sheet.cols > kScreenSnapshotMaxCols || sheet.rows > kTallSnapshotMaxRows)
            {
                out.error = L"the sheet has no usable size";
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
            if (!PickGridFont(dw.Get(), sheet.fontFace, font, out.error))
            {
                return out;
            }
            out.fontFamily = font.family;
            const ScreenImageGrid grid = ComputeScreenImageGrid(sheet.cols, 1, font.advanceEm, font.lineHeightEm, FixedCellOptions(sheet.cols, options));
            if (grid.cellWidthPx <= 0 || grid.cellHeightPx <= 0 || grid.widthPx <= 0)
            {
                out.error = L"the font gives no usable cell";
                return out;
            }
            out.grid = grid;
            out.grid.heightPx = 0; // each part has its own height
            const int padPx = options.paddingPx < 0 ? 0 : options.paddingPx; // the same clamp FixedCellOptions applies
            // How many rows one part holds: the part height less its padding, in whole cells (a part shows at least one row,
            // even when the height asked for is smaller than a single cell).
            const int rowsPerPart = (std::max)(1, ((std::max)(options.maxPartHeightPx, 0) - 2 * padPx) / grid.cellHeightPx);
            const int rowCount = (std::min)(sheet.rows, static_cast<int>(sheet.lines.size()));
            if (rowCount <= 0)
            {
                out.error = L"the sheet has no rows";
                return out;
            }
            const auto plan = PlanTallImageParts(rowCount, rowsPerPart, cuts, maxParts);
            const int widthPx = grid.widthPx;
            // The strip: never taller than the tallest part needs, so a short sheet allocates only what it draws.
            int tallestPx = 0;
            for (const auto& [first, count] : plan)
            {
                tallestPx = (std::max)(tallestPx, count * grid.cellHeightPx + 2 * padPx);
            }
            const int stripPx = (std::max)(1, (std::min)((std::max)(options.stripHeightPx, grid.cellHeightPx), tallestPx));

            const float cellW = static_cast<float>(grid.cellWidthPx);
            const float cellH = static_cast<float>(grid.cellHeightPx);
            const float lineH = grid.lineHeightPx;
            const float fontPx = grid.fontPx;
            const float baseline = static_cast<float>(font.ascentEm * fontPx);
            // The four faces a cell can ask for, indexed by bold (1) | italic (2), on one uniform line (RenderScreenSnapshotPng).
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
            ComPtr<IWICBitmap> strip;
            hr = wic->CreateBitmap(static_cast<UINT>(widthPx), static_cast<UINT>(stripPx), GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &strip);
            if (FAILED(hr))
            {
                out.error = Failed(L"CreateBitmap", hr);
                return out;
            }
            ComPtr<ID2D1Factory> d2d;
            hr = ::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf()); // this call's thread only
            if (FAILED(hr))
            {
                out.error = Failed(L"D2D1CreateFactory", hr);
                return out;
            }
            const D2D1_RENDER_TARGET_PROPERTIES targetProperties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
            ComPtr<ID2D1RenderTarget> target;
            hr = d2d->CreateWicBitmapRenderTarget(strip.Get(), &targetProperties, &target);
            if (FAILED(hr))
            {
                out.error = Failed(L"CreateWicBitmapRenderTarget", hr);
                return out;
            }
            // The screen renderer's settings: grayscale text (a picture is zoomed and recompressed), aliased edges (backgrounds
            // and lines on whole pixels, so neighboring cells never bleed a seam), color emoji through a device context.
            target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
            ComPtr<ID2D1DeviceContext> deviceContext;
            BrushCache brushes;
            brushes.target = target.Get();
            TallPainter painter;
            painter.target = target.Get();
            for (int i = 0; i < 4; ++i)
            {
                painter.formats[i] = formats[i].Get();
            }
            painter.brushes = &brushes;
            painter.cellW = cellW;
            painter.cellH = cellH;
            painter.lineH = lineH;
            painter.lineInset = (cellH - lineH) / 2.0f;
            painter.advancePx = static_cast<float>(font.advanceEm * fontPx);
            painter.pad = static_cast<float>(padPx);
            painter.underlineOffset = baseline - static_cast<float>(font.underlineEm * fontPx); // positions are measured upward
            painter.underlineThickness = (std::max)(1.0f, std::round(static_cast<float>(font.underlineThicknessEm * fontPx)));
            painter.strikeOffset = baseline - static_cast<float>(font.strikeEm * fontPx);
            painter.strikeThickness = (std::max)(1.0f, std::round(static_cast<float>(font.strikeThicknessEm * fontPx)));
            painter.defaultBg = sheet.defaultBg & 0xFFFFFFu;
            painter.textOptions = SUCCEEDED(target.As(&deviceContext)) ? D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT : D2D1_DRAW_TEXT_OPTIONS_NONE;

            // One 24-bit row buffer for a strip: the picture is opaque, so the alpha byte is dropped (a smaller file).
            const UINT rowBytes24 = static_cast<UINT>(widthPx) * 3u;
            std::vector<BYTE> converted(static_cast<size_t>(rowBytes24) * static_cast<size_t>(stripPx));
            for (size_t partIndex = 0; partIndex < plan.size(); ++partIndex)
            {
                const auto [firstRow, partRows] = plan[partIndex];
                TallImagePart part;
                part.firstRow = firstRow;
                part.rowCount = partRows;
                part.widthPx = widthPx;
                part.heightPx = partRows * grid.cellHeightPx + 2 * padPx;

                ComPtr<IStream> stream;
                hr = ::CreateStreamOnHGlobal(nullptr, TRUE, &stream);
                ComPtr<IWICBitmapEncoder> encoder;
                if (SUCCEEDED(hr))
                {
                    hr = wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
                }
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
                    hr = frame->SetSize(static_cast<UINT>(widthPx), static_cast<UINT>(part.heightPx));
                }
                // The rows go in with WritePixels in the frame's own format, so it must be the one asked for: PNG takes 24bppBGR
                // natively, and anything else the encoder might answer with is refused rather than written wrong.
                WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
                if (SUCCEEDED(hr))
                {
                    hr = frame->SetPixelFormat(&format);
                }
                if (SUCCEEDED(hr) && format != GUID_WICPixelFormat24bppBGR)
                {
                    hr = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
                }
                if (FAILED(hr))
                {
                    out.error = Failed(L"starting the PNG of a part", hr);
                    return out;
                }
                // The part strip by strip: each strip draws the rows that touch it (a cell of margin on both sides catches glyphs
                // that reach past their row), then its pixels are appended to the frame. Successive WritePixels calls are
                // sequential scanlines of the frame, so the whole part is never in memory at once.
                for (int stripTop = 0; stripTop < part.heightPx; stripTop += stripPx)
                {
                    const int stripRows = (std::min)(stripPx, part.heightPx - stripTop);
                    const int firstTouched = (std::max)(0, (stripTop - padPx) / grid.cellHeightPx - 1);
                    const int lastTouched = (std::min)(partRows, (stripTop + stripRows - padPx) / grid.cellHeightPx + 2);
                    target->BeginDraw();
                    target->Clear(D2D1::ColorF(sheet.defaultBg & 0xFFFFFFu));
                    for (int r = firstTouched; r < lastTouched; ++r)
                    {
                        const float top = static_cast<float>(padPx + r * grid.cellHeightPx - stripTop);
                        painter.Row(sheet.lines[static_cast<size_t>(firstRow + r)], top);
                    }
                    hr = target->EndDraw();
                    if (FAILED(hr))
                    {
                        out.error = Failed(L"EndDraw", hr);
                        return out;
                    }
                    // Read the strip back (it is unlocked again before the next BeginDraw: Direct2D cannot draw into a locked
                    // bitmap) and drop the alpha byte of every pixel (the strip is opaque: it was cleared with an opaque color).
                    {
                        const WICRect rect{ 0, 0, widthPx, stripRows };
                        ComPtr<IWICBitmapLock> lock;
                        hr = strip->Lock(&rect, WICBitmapLockRead, &lock);
                        UINT stride = 0;
                        UINT size = 0;
                        BYTE* pixels = nullptr;
                        if (SUCCEEDED(hr))
                        {
                            hr = lock->GetStride(&stride);
                        }
                        if (SUCCEEDED(hr))
                        {
                            hr = lock->GetDataPointer(&size, &pixels);
                        }
                        if (SUCCEEDED(hr) && (!pixels || stride < static_cast<UINT>(widthPx) * 4u || static_cast<size_t>(size) < static_cast<size_t>(stride) * static_cast<size_t>(stripRows - 1) + static_cast<size_t>(widthPx) * 4u))
                        {
                            hr = E_UNEXPECTED; // a lock smaller than the rect it was asked for: never read past it
                        }
                        if (FAILED(hr))
                        {
                            out.error = Failed(L"reading a drawn strip", hr);
                            return out;
                        }
                        for (int y = 0; y < stripRows; ++y)
                        {
                            const BYTE* src = pixels + static_cast<size_t>(y) * stride;
                            BYTE* dst = converted.data() + static_cast<size_t>(y) * rowBytes24;
                            for (int x = 0; x < widthPx; ++x)
                            {
                                dst[0] = src[0];
                                dst[1] = src[1];
                                dst[2] = src[2];
                                src += 4;
                                dst += 3;
                            }
                        }
                    }
                    hr = frame->WritePixels(static_cast<UINT>(stripRows), rowBytes24, rowBytes24 * static_cast<UINT>(stripRows), converted.data());
                    if (FAILED(hr))
                    {
                        out.error = Failed(L"WritePixels", hr);
                        return out;
                    }
                }
                hr = frame->Commit();
                if (SUCCEEDED(hr))
                {
                    hr = encoder->Commit();
                }
                if (FAILED(hr))
                {
                    out.error = Failed(L"the PNG encoding", hr);
                    return out;
                }
                if (!CopyStreamBytes(stream.Get(), part.png, out.error))
                {
                    return out;
                }
                if (onPart)
                {
                    // The caller takes the bytes now (it sends part 1 while part 2 is drawn); the metadata stays in `parts`,
                    // copied field by field so the megabytes of PNG are never duplicated.
                    TallImagePart metadata;
                    metadata.firstRow = part.firstRow;
                    metadata.rowCount = part.rowCount;
                    metadata.widthPx = part.widthPx;
                    metadata.heightPx = part.heightPx;
                    onPart(std::move(part), partIndex, plan.size());
                    out.parts.push_back(std::move(metadata));
                }
                else
                {
                    out.parts.push_back(std::move(part));
                }
            }
            return out;
        }
        catch (...)
        {
            // Rule #18: the throw site goes to hooks.log; the caller gets a reason, and keeps every part finished before it.
            LogSwallowedException(L"RenderTallScreenSnapshotPngs");
            out.error = L"an exception while drawing the sheet";
            return out;
        }
    }
}
