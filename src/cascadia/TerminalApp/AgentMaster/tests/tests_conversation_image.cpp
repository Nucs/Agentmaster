// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster M5 — MESSAGING.md §21.39: 📜 All Screen, a session's WHOLE conversation drawn as tall pictures —
//   1. the part planner (ScreenImage.h PlanTallImageParts): parts cover every row once, start at a turn or a block where one
//      is near, and fall back to full parts when the good places would need more parts than allowed;
//   2. the tall renderer (RenderTallScreenSnapshotPngs): REAL PNGs drawn with Direct2D + DirectWrite + WIC, decoded again —
//      the parts' sizes, a colored cell in each part at its row, no seam between strips (two strip heights give the same
//      pixels), onPart taking the bytes, and runs of ASCII drawn in one call giving the same pixels as the screen renderer's
//      cell-by-cell drawing;
//   3. the layout (ConversationImage.h BuildConversationSheet) on the turns view's own goldens (tests\turns\expected) and on a
//      hand-written document: the turn rule, the prompt band, the bullets' colors, the hidden labels, an agent's indent, the
//      "⎿" rows, wrapping (words whole, a long word split, wide glyphs), a code block's band and spaces, the row cap's note;
//      and a ```diff block as "File diffs" draws it (3c): the gutter's numbers counted from the hunk headers, the green and red
//      bands, the ellipsis between hunks, a quoted git diff's dim file header vs a lone "--- " line (a removed "-- " line),
//      a "\ No newline" note, a long line broken at the edge on its band;
//   4. end to end (RenderConversationImages) on a turns fixture transcript, and again with File diffs (two more rows).
// Sections 3 and 4 need MarkdownBlocks (cmark): without the vcpkg cmark of a full build they report a skip, like
// TestMarkdownBlocks. The service's half (the button, the worker thread, sendDocument) needs Telegram: the live check is the
// dev bot (MESSAGING.md §21.39 "Check after the build").

#include "m5_tests.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm> // (std::max) / (std::min): the <windows.h> macros stay out of the way
#include <cstring> // std::memcmp (the PNG signature)
#include <fstream>
#include <iterator>
#include <utility>
#include <vector>

#include "../ConversationImage.h"
#include "../MarkdownBlocks.h"
#include "../ScreenImage.h"
#include "../ScreenSnapshot.h"

using namespace Agentmaster;

namespace
{
    namespace fs = std::filesystem;

    /// <summary>One cell of a test sheet.</summary>
    /// <param name="text">Its text.</param>
    /// <param name="fg">Its foreground (0xRRGGBB).</param>
    /// <param name="bg">Its background (0xRRGGBB).</param>
    /// <param name="flags">kScreenCell* bits.</param>
    /// <param name="width">1, or 2 for a wide glyph.</param>
    /// <returns>The cell.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be stored.</exception>
    ScreenCell ConvCell(std::wstring text, uint32_t fg, uint32_t bg, uint8_t flags = 0, uint8_t width = 1)
    {
        ScreenCell c;
        c.text = std::move(text);
        c.fg = fg;
        c.bg = bg;
        c.flags = flags;
        c.width = width;
        return c;
    }

    /// <summary>A row of one-column cells in one style, one per character of `text` (ASCII in every use here).</summary>
    /// <param name="text">The row's text.</param>
    /// <param name="fg">The foreground.</param>
    /// <param name="bg">The background.</param>
    /// <param name="flags">kScreenCell* bits of every cell.</param>
    /// <returns>The cells.</returns>
    /// <exception cref="std::bad_alloc">If the row cannot be allocated.</exception>
    std::vector<ScreenCell> AsciiRow(std::wstring_view text, uint32_t fg, uint32_t bg, uint8_t flags = 0)
    {
        std::vector<ScreenCell> row;
        for (const wchar_t c : text)
        {
            row.push_back(ConvCell(std::wstring(1, c), fg, bg, flags));
        }
        return row;
    }

    /// <summary>A decoded picture: 32-bit BGRA rows, top-down.</summary>
    struct ConvDecoded
    {
        UINT width{ 0 }; // in pixels
        UINT height{ 0 }; // in pixels
        std::vector<uint8_t> bgra; // width x height x 4 bytes, no row padding

        /// <summary>The pixel at (x, y).</summary>
        /// <param name="x">The column (below width: not checked).</param>
        /// <param name="y">The row (below height: not checked).</param>
        /// <returns>Its color as 0xRRGGBB.</returns>
        uint32_t At(UINT x, UINT y) const
        {
            const size_t i = (static_cast<size_t>(y) * width + x) * 4;
            return (static_cast<uint32_t>(bgra[i + 2]) << 16) | (static_cast<uint32_t>(bgra[i + 1]) << 8) | bgra[i];
        }
    };

    /// <summary>Decodes PNG bytes with WIC into 32bppBGRA: another code path than the renderer's encoder reads the picture back.</summary>
    /// <param name="png">The file's bytes.</param>
    /// <param name="out">The pixels (left as they are on failure).</param>
    /// <returns>True when the bytes decoded.</returns>
    /// <exception cref="std::bad_alloc">If the pixels cannot be allocated.</exception>
    bool ConvDecodePng(const std::string& png, ConvDecoded& out)
    {
        using Microsoft::WRL::ComPtr;
        const HRESULT co = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool ok = false;
        {
            ComPtr<IWICImagingFactory> wic;
            ComPtr<IWICStream> stream;
            ComPtr<IWICBitmapDecoder> decoder;
            ComPtr<IWICBitmapFrameDecode> frame;
            ComPtr<IWICFormatConverter> converter;
            if (!png.empty() && SUCCEEDED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) && SUCCEEDED(wic->CreateStream(&stream)) &&
                SUCCEEDED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(png.data())), static_cast<DWORD>(png.size()))) &&
                SUCCEEDED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) && SUCCEEDED(decoder->GetFrame(0, &frame)) &&
                SUCCEEDED(wic->CreateFormatConverter(&converter)) &&
                SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)) &&
                SUCCEEDED(converter->GetSize(&out.width, &out.height)) && out.width > 0 && out.height > 0)
            {
                out.bgra.resize(static_cast<size_t>(out.width) * out.height * 4);
                ok = SUCCEEDED(converter->CopyPixels(nullptr, out.width * 4, static_cast<UINT>(out.bgra.size()), out.bgra.data()));
            }
        }
        if (SUCCEEDED(co))
        {
            ::CoUninitialize();
        }
        return ok;
    }

    /// <summary>Counts the pixels two pictures disagree on (the color channels only).</summary>
    /// <param name="a">One picture.</param>
    /// <param name="b">The other.</param>
    /// <returns>How many pixels differ; -1 when the sizes differ.</returns>
    long long ConvDiffPixels(const ConvDecoded& a, const ConvDecoded& b)
    {
        if (a.width != b.width || a.height != b.height || a.bgra.size() != b.bgra.size())
        {
            return -1;
        }
        long long n = 0;
        for (size_t i = 0; i < a.bgra.size(); i += 4)
        {
            n += (a.bgra[i] != b.bgra[i] || a.bgra[i + 1] != b.bgra[i + 1] || a.bgra[i + 2] != b.bgra[i + 2]) ? 1 : 0;
        }
        return n;
    }

#ifdef AM_HAVE_CMARK
    /// <summary>The folder m5_tests.exe lives in: the turns fixtures and goldens ship beside it (tests\turns).</summary>
    /// <returns>The folder; "." when the module path cannot be read.</returns>
    fs::path ConvHarnessDir()
    {
        wchar_t buffer[MAX_PATH * 4]{};
        const DWORD n = ::GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
        if (n == 0 || n >= std::size(buffer))
        {
            return fs::path{ L"." };
        }
        return fs::path{ buffer }.parent_path();
    }

    /// <summary>A UTF-8 file as UTF-16.</summary>
    /// <param name="path">The file.</param>
    /// <returns>Its text; "" when it cannot be read (the caller's checks then fail visibly).</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    std::wstring ConvReadUtf8File(const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (bytes.empty())
        {
            return {};
        }
        const int n = ::MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
        std::wstring text(static_cast<size_t>((std::max)(n, 0)), L'\0');
        if (n > 0)
        {
            ::MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), text.data(), n);
        }
        return text;
    }

    /// <summary>A sheet row's text: its cells' text joined.</summary>
    /// <param name="row">The row.</param>
    /// <returns>The text.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    std::wstring ConvRowText(const std::vector<ScreenCell>& row)
    {
        std::wstring text;
        for (const auto& c : row)
        {
            text += c.text;
        }
        return text;
    }

    /// <summary>The columns a sheet row covers.</summary>
    /// <param name="row">The row.</param>
    /// <returns>The sum of its cells' widths (a wide cell counts 2).</returns>
    int ConvRowWidth(const std::vector<ScreenCell>& row)
    {
        int w = 0;
        for (const auto& c : row)
        {
            w += c.width == 2 ? 2 : 1;
        }
        return w;
    }

    /// <summary>Finds the first row whose text contains `needle`.</summary>
    /// <param name="sheet">The sheet.</param>
    /// <param name="needle">The text to find.</param>
    /// <param name="from">The first row to look at.</param>
    /// <returns>The row's index, or -1.</returns>
    /// <exception cref="std::bad_alloc">If a row's text cannot be built.</exception>
    int ConvFindRow(const ConversationSheet& sheet, std::wstring_view needle, int from = 0)
    {
        for (int r = (std::max)(0, from); r < static_cast<int>(sheet.snapshot.lines.size()); ++r)
        {
            if (ConvRowText(sheet.snapshot.lines[static_cast<size_t>(r)]).find(needle) != std::wstring::npos)
            {
                return r;
            }
        }
        return -1;
    }

    /// <summary>The cell where `needle` starts in a row (found in the row's text, its cells' text joined), so its colors can be read.</summary>
    /// <param name="row">The row.</param>
    /// <param name="needle">The text to find.</param>
    /// <returns>The index of the cell holding the needle's first character, or -1 when the row does not hold it.</returns>
    /// <exception cref="std::bad_alloc">If the row's text cannot be built.</exception>
    int ConvCellIndexOf(const std::vector<ScreenCell>& row, std::wstring_view needle)
    {
        const size_t at = ConvRowText(row).find(needle);
        if (at == std::wstring::npos)
        {
            return -1;
        }
        // A cell may hold more than one UTF-16 unit (a surrogate pair, a combining mark): walk the cells, not the text.
        size_t units = 0;
        for (size_t i = 0; i < row.size(); ++i)
        {
            if (units == at)
            {
                return static_cast<int>(i);
            }
            units += row[i].text.size();
        }
        return -1;
    }

    /// <summary>Parses a turns-view document the way RenderConversationImages parses it and lays it out.</summary>
    /// <param name="markdown">The document.</param>
    /// <param name="cols">The sheet's width.</param>
    /// <param name="maxRows">The row cap (0 = none).</param>
    /// <returns>The sheet.</returns>
    /// <exception cref="std::bad_alloc">If the sheet cannot be allocated.</exception>
    ConversationSheet ConvLayout(std::wstring_view markdown, int cols, int maxRows = 0)
    {
        MdParseOptions parse;
        parse.softBreaksAsLineBreaks = true;
        parse.maxBlockChars = kConversationBlockMaxChars;
        const MdParseResult parsed = ParseMarkdownBlocks(markdown, parse);
        ConversationSheetOptions options;
        options.cols = cols;
        options.maxRows = maxRows;
        return BuildConversationSheet(parsed.blocks, options);
    }
#endif
}

void TestConversationImage()
{
    // ASCII only, like the other groups' headers: the console's code page would garble a section sign.
    std::wprintf(L"== TestConversationImage (MESSAGING.md 21.39: All Screen - the part planner, the tall renderer, the layout, end to end) ==\n");

    // ===== 1. the part planner =====
    {
        CHECK(PlanTallImageParts(0, 10, {}).empty(), "planner: a sheet with no rows has no parts");
        const auto one = PlanTallImageParts(7, 10, {});
        CHECK(one.size() == 1 && one[0] == std::make_pair(0, 7), "planner: a sheet within one part is one part");
        const auto hard = PlanTallImageParts(25, 10, {});
        CHECK(hard.size() == 3 && hard[0] == std::make_pair(0, 10) && hard[1] == std::make_pair(10, 10) && hard[2] == std::make_pair(20, 5),
              "planner: with no good place to cut, every part but the last is full");

        std::vector<uint8_t> turnCuts(25, kTallCutAny);
        turnCuts[7] = kTallCutTurn; // inside the first window's last 40% (rows 6..10)
        const auto turn = PlanTallImageParts(25, 10, turnCuts);
        CHECK(!turn.empty() && turn[0] == std::make_pair(0, 7), "planner: a part ends right before a turn in the last 40% of its room");

        std::vector<uint8_t> blockCuts(25, kTallCutAny);
        blockCuts[3] = kTallCutTurn; // too early: a part that short would waste 70% of its room
        blockCuts[9] = kTallCutBlock; // inside the last 15% (rows 8..10)
        const auto block = PlanTallImageParts(25, 10, blockCuts);
        CHECK(!block.empty() && block[0] == std::make_pair(0, 9), "planner: no turn near the end, so a block in the last 15% of the room");

        std::vector<uint8_t> mixed(1000, kTallCutAny);
        for (size_t i = 0; i < mixed.size(); ++i)
        {
            mixed[i] = i % 37 == 0 ? kTallCutTurn : (i % 5 == 0 ? kTallCutBlock : kTallCutAny);
        }
        const auto plan = PlanTallImageParts(1000, 64, mixed);
        bool contiguous = true;
        bool sized = true;
        int next = 0;
        for (const auto& [first, count] : plan)
        {
            contiguous = contiguous && first == next;
            sized = sized && count >= 1 && count <= 64;
            next = first + count;
        }
        CHECK(contiguous && sized && next == 1000, "planner: the parts cover every row once, in order, none empty and none past its room");

        std::vector<uint8_t> everySeventh(100, kTallCutAny);
        for (size_t i = 0; i < everySeventh.size(); i += 7)
        {
            everySeventh[i] = kTallCutTurn;
        }
        const auto preferring = PlanTallImageParts(100, 10, everySeventh);
        const auto capped = PlanTallImageParts(100, 10, everySeventh, 10);
        CHECK(preferring.size() > 10 && capped.size() == 10, "planner: when the good places would need more parts than allowed, every part is filled instead");
        CHECK(PlanTallImageParts(5, 0, {}).size() == 5, "planner: a part holds at least one row");
    }

    // ===== 2. the tall renderer =====
    {
        TallScreenImageOptions o;
        o.cellWidthPx = 10;
        o.paddingPx = 8;
        std::wstring family;
        std::wstring error;
        const ScreenImageGrid g = MeasureTallScreenImageGrid(L"", 40, o, family, error);
        CHECK(g.cellWidthPx == 10 && g.cellHeightPx > 0 && g.widthPx == 40 * 10 + 16 && !family.empty() && error.empty(),
              "measure: the fixed cell, a cell height, the width with its padding, a monospace font");
        std::wstring noFamily;
        std::wstring noError;
        CHECK(MeasureTallScreenImageGrid(L"", 0, o, noFamily, noError).cellWidthPx == 0 && !noError.empty(), "measure: a sheet with no columns is refused with a reason");
        const int ch = (std::max)(1, g.cellHeightPx);
        o.maxPartHeightPx = 100 * ch + 16; // exactly 100 rows a part

        ScreenSnapshot s;
        s.cols = 40;
        s.rows = 300;
        s.defaultFg = 0xC0C0C0;
        s.defaultBg = 0x101010;
        s.lines.resize(300);
        for (int r = 0; r < 300; ++r)
        {
            s.lines[static_cast<size_t>(r)] = AsciiRow(L"row " + std::to_wstring(r) + L" of the tall sheet", 0xC0C0C0, 0x101010);
        }
        s.lines[0][0].bg = 0xFF0000; // part 1's first cell
        for (int c = 0; c < 6; ++c)
        {
            s.lines[150].push_back(ConvCell(L" ", 0xC0C0C0, c == 5 ? 0x00FF00 : 0x101010)); // part 2, its row 50, column (row text + 5)
        }
        s.lines[299].clear();
        for (int c = 0; c < 40; ++c)
        {
            s.lines[299].push_back(ConvCell(L" ", 0xC0C0C0, c == 39 ? 0x0000FF : 0x101010)); // part 3's last row, last column
        }
        const int greenColumn = static_cast<int>(s.lines[150].size()) - 1;

        const TallScreenImage img = RenderTallScreenSnapshotPngs(s, {}, 0, o);
        CHECK(img.error.empty(), "tall: the sheet is drawn (no error)");
        if (!img.error.empty())
        {
            std::wprintf(L"    tall render error: %ls\n", img.error.c_str());
        }
        CHECK(img.parts.size() == 3, "tall: 300 rows at 100 rows a part are three parts");
        bool shaped = img.parts.size() == 3;
        for (size_t p = 0; p < img.parts.size(); ++p)
        {
            const TallImagePart& part = img.parts[p];
            shaped = shaped && part.firstRow == static_cast<int>(p) * 100 && part.rowCount == 100 && part.widthPx == 416 && part.heightPx == 100 * ch + 16 &&
                     part.png.size() > 64 && std::memcmp(part.png.data(), "\x89PNG\r\n\x1a\n", 8) == 0;
        }
        CHECK(shaped, "tall: each part shows its 100 rows at the sheet's width, as a PNG");
        std::wprintf(L"    drawn in %ls: 3 parts of %dx%d px, cell %dx%d px\n", img.fontFamily.c_str(), img.grid.widthPx, 100 * ch + 16, img.grid.cellWidthPx, img.grid.cellHeightPx);
        ConvDecoded d[3];
        bool decoded = img.parts.size() == 3;
        for (size_t p = 0; decoded && p < 3; ++p)
        {
            decoded = ConvDecodePng(img.parts[p].png, d[p]) && d[p].width == 416u && d[p].height == static_cast<UINT>(100 * ch + 16);
        }
        CHECK(decoded, "tall: every part decodes again, at its size");
        if (decoded)
        {
            CHECK(d[0].At(2, 2) == 0x101010 && d[1].At(2, 2) == 0x101010 && d[2].At(415, static_cast<UINT>(100 * ch + 15)) == 0x101010, "tall: each part's padding is the sheet's background");
            CHECK(d[0].At(8 + 1, 8 + 1) == 0xFF0000, "tall: part 1 starts with the sheet's first row (its red cell)");
            CHECK(d[1].At(static_cast<UINT>(8 + greenColumn * 10 + 5), static_cast<UINT>(8 + 50 * ch + ch / 2)) == 0x00FF00, "tall: part 2's row 50 is the sheet's row 150 (its green cell)");
            CHECK(d[2].At(static_cast<UINT>(8 + 39 * 10 + 5), static_cast<UINT>(8 + 99 * ch + ch / 2)) == 0x0000FF, "tall: part 3 ends with the sheet's last row (its blue cell in the last column)");
            int ink = 0;
            for (int y = 8 + 10 * ch; y < 8 + 11 * ch; ++y)
            {
                for (int x = 8; x < 8 + 3 * 10; ++x)
                {
                    ink += d[1].At(static_cast<UINT>(x), static_cast<UINT>(y)) != 0x101010 ? 1 : 0;
                }
            }
            CHECK(ink > 10, "tall: a row's ASCII text is drawn (ink in its first cells)");
        }

        // No seam between strips: a 37 px strip (many boundaries, none on a row edge) and one strip for the whole part draw
        // the same pixels.
        TallScreenImageOptions narrowStrip = o;
        narrowStrip.stripHeightPx = 37;
        TallScreenImageOptions wholeStrip = o;
        wholeStrip.stripHeightPx = 100000;
        const TallScreenImage a = RenderTallScreenSnapshotPngs(s, {}, 0, narrowStrip);
        const TallScreenImage b = RenderTallScreenSnapshotPngs(s, {}, 0, wholeStrip);
        ConvDecoded da;
        ConvDecoded db;
        const bool both = a.parts.size() == 3 && b.parts.size() == 3 && ConvDecodePng(a.parts[1].png, da) && ConvDecodePng(b.parts[1].png, db);
        CHECK(both && ConvDiffPixels(da, db) == 0, "tall: a 37 px strip and a whole-part strip give identical pixels (no seam where strips meet)");

        // onPart takes each part's bytes as soon as it is encoded; the result keeps the metadata.
        size_t calls = 0;
        size_t bytes = 0;
        bool indexed = true;
        const TallScreenImage handed = RenderTallScreenSnapshotPngs(s, {}, 0, o, [&](TallImagePart&& part, size_t index, size_t count) {
            indexed = indexed && index == calls && count == 3 && part.firstRow == static_cast<int>(index) * 100;
            ++calls;
            bytes += part.png.size();
        });
        CHECK(calls == 3 && indexed && bytes > 0, "tall: onPart gets every part in order, with its bytes");
        CHECK(handed.parts.size() == 3 && handed.parts[0].png.empty() && handed.parts[2].rowCount == 100, "tall: with onPart the result keeps each part's rows and size, not its bytes");

        // ASCII runs drawn in ONE call land exactly where the screen renderer's cell-by-cell drawing puts them: one sheet, both
        // renderers, the same cell. Two colors, bold, italic, underline, a box rule (stretched, alone) and a wide glyph (alone).
        ScreenSnapshot mixedSheet;
        mixedSheet.cols = 30;
        mixedSheet.rows = 4;
        mixedSheet.defaultFg = 0xC0C0C0;
        mixedSheet.defaultBg = 0x101010;
        mixedSheet.lines.resize(4);
        mixedSheet.lines[0] = AsciiRow(L"plain text, then", 0xC0C0C0, 0x101010);
        for (auto& c : AsciiRow(L" bold", 0xFFFFFF, 0x101010, kScreenCellBold))
        {
            mixedSheet.lines[0].push_back(std::move(c));
        }
        mixedSheet.lines[1] = AsciiRow(L"italic and underlined", 0x80C0FF, 0x202020, kScreenCellItalic | kScreenCellUnderline);
        mixedSheet.lines[2] = AsciiRow(L"box:", 0xC0C0C0, 0x101010);
        mixedSheet.lines[2].push_back(ConvCell(L"\x2500", 0xFFFF00, 0x101010));
        mixedSheet.lines[2].push_back(ConvCell(L"\x2500", 0xFFFF00, 0x101010));
        mixedSheet.lines[2].push_back(ConvCell(L" ", 0xC0C0C0, 0x101010));
        mixedSheet.lines[2].push_back(ConvCell(L"\x4E2D", 0xC0C0C0, 0x101010, 0, 2));
        mixedSheet.lines[3] = AsciiRow(L"  leading and trailing  ", 0xC0C0C0, 0x101010);
        ScreenImageOptions screenOptions;
        screenOptions.paddingPx = 8;
        screenOptions.minCellWidthPx = 10;
        screenOptions.maxCellWidthPx = 10;
        screenOptions.maxWidthPx = 30 * 10 + 16;
        TallScreenImageOptions tallOptions;
        tallOptions.cellWidthPx = 10;
        tallOptions.paddingPx = 8;
        const ScreenImage byCell = RenderScreenSnapshotPng(mixedSheet, screenOptions);
        const TallScreenImage byRun = RenderTallScreenSnapshotPngs(mixedSheet, {}, 0, tallOptions);
        ConvDecoded dc;
        ConvDecoded dr;
        const bool pair = byCell.error.empty() && byRun.parts.size() == 1 && ConvDecodePng(byCell.png, dc) && ConvDecodePng(byRun.parts[0].png, dr);
        const long long differing = pair ? ConvDiffPixels(dc, dr) : -1;
        std::wprintf(L"    batched runs vs cell by cell: %lld differing pixels of %u\n", differing, pair ? dc.width * dc.height : 0u);
        CHECK(pair && differing == 0, "tall: ASCII runs drawn in one call give the very pixels of the screen renderer's cell-by-cell drawing");

        ScreenSnapshot none;
        const TallScreenImage refused = RenderTallScreenSnapshotPngs(none, {}, 0, o);
        CHECK(refused.parts.empty() && !refused.error.empty(), "tall: a sheet with no size is refused with a reason");
        ScreenSnapshot huge;
        huge.cols = 10;
        huge.rows = kTallSnapshotMaxRows + 1;
        const TallScreenImage tooTall = RenderTallScreenSnapshotPngs(huge, {}, 0, o);
        CHECK(tooTall.parts.empty() && !tooTall.error.empty(), "tall: a sheet past kTallSnapshotMaxRows is refused before anything is drawn");
    }

#ifdef AM_HAVE_CMARK
    // ===== 3. the layout, on the turns view's own golden (an agents session) =====
    {
        const fs::path golden = ConvHarnessDir() / L"turns" / L"expected" / L"agents-c1c1c1c1-default.md";
        const std::wstring markdown = ConvReadUtf8File(golden);
        CHECK(!markdown.empty(), "layout: the agents golden is beside the exe (tests\\turns\\expected)");
        const ConversationSheet sheet = ConvLayout(markdown, kConversationImageCols);
        const auto& lines = sheet.snapshot.lines;
        CHECK(sheet.snapshot.cols == kConversationImageCols && sheet.snapshot.rows == static_cast<int>(lines.size()) && sheet.cuts.size() == lines.size() &&
                  sheet.rowTurn.size() == lines.size() && sheet.droppedRows == 0,
              "layout: one cut value and one turn per row, nothing left out without a cap");
        bool fits = true;
        for (const auto& row : lines)
        {
            fits = fits && ConvRowWidth(row) <= kConversationImageCols;
        }
        CHECK(fits, "layout: no row is wider than the sheet");

        // The turn rule: "── Turn 1 · ... ────", the number in Claude's orange, the rule to the edge.
        CHECK(!lines.empty() && lines[0].size() > 4 && lines[0][0].text == L"\x2500" && lines[0][1].text == L"\x2500" && ConvRowWidth(lines[0]) == kConversationImageCols &&
                  lines[0].back().text == L"\x2500",
              "layout: a turn opens with a rule across the sheet");
        CHECK(!lines.empty() && lines[0].size() > 4 && lines[0][3].text == L"T" && lines[0][3].fg == kConvClaude && (lines[0][3].flags & kScreenCellBold) != 0,
              "layout: the turn's number is bold, in Claude's orange");
        CHECK(!sheet.cuts.empty() && sheet.cuts[0] == kTallCutTurn, "layout: a part may best start at a turn's rule");

        // The prompt: "❯ " and the text on the gray band, across the whole row.
        const int prompt = ConvFindRow(sheet, L"review the commit with a team");
        CHECK(prompt > 0 && lines[static_cast<size_t>(prompt)][0].text == L"\x276F" && lines[static_cast<size_t>(prompt)][0].bg == kConvPromptBand &&
                  ConvRowWidth(lines[static_cast<size_t>(prompt)]) == kConversationImageCols && lines[static_cast<size_t>(prompt)].back().bg == kConvPromptBand,
              "layout: the user's message is on the gray band after the prompt mark, the band across the row");
        CHECK(ConvFindRow(sheet, L"Claude") < 0 && ConvFindRow(sheet, L"\x276F User") < 0, "layout: the User and Claude labels draw no line of their own");

        // Claude's work: the remark's emoji goes (the bullet says it), the calls' bullets wear how they ended.
        const int remark = ConvFindRow(sheet, L"Spawning two finders");
        CHECK(remark > 0 && lines[static_cast<size_t>(remark)][0].text == L"\x25CF" && lines[static_cast<size_t>(remark)][0].fg == kConvBright &&
                  ConvRowText(lines[static_cast<size_t>(remark)]).find(L"\U0001F4AC") == std::wstring::npos,
              "layout: a remark is Claude's white bullet, without the turns view's speech-bubble emoji");
        const int ok = ConvFindRow(sheet, L"Finder A: scan the diff (general-purpose)");
        CHECK(ok > 0 && lines[static_cast<size_t>(ok)][0].fg == kConvOk && lines[static_cast<size_t>(ok)][2].text == L"A" && (lines[static_cast<size_t>(ok)][2].flags & kScreenCellBold) != 0,
              "layout: a call that ended well wears the green bullet, its tool's name bold");
        const int pending = ConvFindRow(sheet, L"Finder B: audit (general-purpose)");
        CHECK(pending > 0 && lines[static_cast<size_t>(pending)][0].fg == kConvPending, "layout: a call with no result wears the gray bullet");

        // The reply: Claude's white bullet, on the plain background.
        const int reply = ConvFindRow(sheet, L"finder-a and finder-b are running");
        CHECK(reply > 0 && lines[static_cast<size_t>(reply)][0].text == L"\x25CF" && lines[static_cast<size_t>(reply)][0].fg == kConvBright && lines[static_cast<size_t>(reply)][0].bg == kConvBg,
              "layout: the reply follows Claude's white bullet");

        // An agent's entry: the orange bullet, its work one step in.
        const int agent = ConvFindRow(sheet, L"teammate \x00B7 Finder A");
        CHECK(agent > 0 && lines[static_cast<size_t>(agent)][0].text == L"\x25CF" && lines[static_cast<size_t>(agent)][0].fg == kConvClaude,
              "layout: an agent's header wears Claude's orange bullet");
        CHECK(agent > 0 && static_cast<size_t>(agent + 1) < lines.size() && lines[static_cast<size_t>(agent + 1)].size() > 3 && lines[static_cast<size_t>(agent + 1)][0].text == L" " &&
                  lines[static_cast<size_t>(agent + 1)][2].text == L"\x25CF" && lines[static_cast<size_t>(agent + 1)][2].fg == kConvPeer,
              "layout: the agent's first item (a message that reached it) is indented under its header, with the message bullet");
        const int midWork = ConvFindRow(sheet, L"Its transcript ends mid-work");
        CHECK(midWork > 0 && lines[static_cast<size_t>(midWork)].size() > 3 && (lines[static_cast<size_t>(midWork)][2].flags & kScreenCellItalic) != 0 &&
                  lines[static_cast<size_t>(midWork)][2].fg == kConvDim,
              "layout: the turns view's note under an agent is dim italic, in the agent's indent");

        // Turn 2: its own rule, every row after it counted in turn 2, the peer's labels in the peer's color.
        const int turn2 = ConvFindRow(sheet, L"Turn 2");
        CHECK(turn2 > 0 && sheet.cuts[static_cast<size_t>(turn2)] == kTallCutTurn && sheet.rowTurn[static_cast<size_t>(turn2)] == 2 && sheet.rowTurn[static_cast<size_t>(turn2) - 1] == 1,
              "layout: the rows of turn 2 start at its rule");
        const int peer = ConvFindRow(sheet, L"\x2709 finder-a");
        CHECK(peer > turn2 && lines[static_cast<size_t>(peer)][0].fg == kConvPeer && (lines[static_cast<size_t>(peer)][0].flags & kScreenCellBold) != 0,
              "layout: a teammate's label is drawn in the peer's color");
        const int peerMessage = ConvFindRow(sheet, L"is idle (available)");
        CHECK(peerMessage > peer && lines[static_cast<size_t>(peerMessage)][0].text == L"\x276F" && lines[static_cast<size_t>(peerMessage)][0].fg == kConvPeer &&
                  lines[static_cast<size_t>(peerMessage)][0].bg == kConvPromptBand,
              "layout: a teammate's message is on the band, its mark in the peer's color");
        CHECK(sheet.turns >= 2, "layout: the sheet counts the turns it laid out");
    }

    // ===== 3b. the layout, on a hand-written document: a failed call, a result row, wrapping, wide glyphs, code, the cap =====
    {
        const std::wstring doc =
            L"### Turn 1 \x00B7 now\n\n"
            L"**\x276F User**\n> short prompt\n\n"
            L"**\x25CF Claude**\n"
            L"- **Bash** \x2014 run: `make` \x00B7 \x2717 exit 2\n"
            L"  - \x21E2 error: something failed\n\n"
            L"> A paragraph long enough to wrap at the sheet's width, made of words that each fit on a row, so none is split.\n"
            L">\n"
            L"> AVeryLongWordWithoutAnySpaceThatIsLongerThanTheWholeSheetWidthAndMustBeSplitAcrossRowsBecauseNothingElseWorks\n"
            L">\n"
            L"> \x4E2D\x6587\x5B57\x7B26 mixed with ASCII text that wraps between wide cells.\n"
            L">\n"
            L"> ```\n"
            L">   indented code    with   spaces\n"
            L"> ```\n\n"
            L"### Turn 2 \x00B7 later\n\n"
            L"**\x276F User, typed while it worked (10:30)**\n> more\n\n"
            L"**\x25CF Claude**\n"
            L"> done\n\n"
            L"### Turn 3 \x00B7 last\n\n"
            L"**\x276F User**\n> nothing to run here\n\n"
            L"**\x25CF Claude**\n"
            L"- (no tool calls)\n\n"
            L"> answered without a tool\n";
        const int cols = 40;
        const ConversationSheet sheet = ConvLayout(doc, cols);
        const auto& lines = sheet.snapshot.lines;
        bool fits = true;
        for (const auto& row : lines)
        {
            fits = fits && ConvRowWidth(row) <= cols;
        }
        CHECK(!lines.empty() && fits, "layout: a 40-column sheet keeps every row within 40 columns");
        const int failed = ConvFindRow(sheet, L"run: make");
        CHECK(failed > 0 && lines[static_cast<size_t>(failed)][0].fg == kConvFail, "layout: a call that failed wears the red bullet");
        const int result = ConvFindRow(sheet, L"error: something failed");
        CHECK(result > failed && lines[static_cast<size_t>(result)].size() > 3 && lines[static_cast<size_t>(result)][2].text == L"\x23BF" && lines[static_cast<size_t>(result)][2].fg == kConvDim,
              "layout: what a call returned hangs under it after the result corner, dimmed");
        // Every word of the paragraph sits whole on one row (wrapping happens between words).
        bool whole = true;
        for (const wchar_t* word : { L"paragraph", L"enough", L"sheet's", L"width,", L"each", L"split." })
        {
            whole = whole && ConvFindRow(sheet, word) >= 0;
        }
        CHECK(whole, "layout: a paragraph wraps between its words, never inside one");
        const int longStart = ConvFindRow(sheet, L"AVeryLongWord");
        CHECK(longStart > 0 && ConvFindRow(sheet, L"NothingElseWorks") > longStart, "layout: a word longer than a row is split across rows");
        const int wide = ConvFindRow(sheet, L"\x4E2D\x6587");
        bool wideCells = false;
        if (wide > 0)
        {
            for (const auto& c : lines[static_cast<size_t>(wide)])
            {
                wideCells = wideCells || (c.text == L"\x4E2D" && c.width == 2);
            }
        }
        CHECK(wideCells, "layout: a CJK character takes two columns");
        const int code = ConvFindRow(sheet, L"  indented code    with   spaces");
        CHECK(code > 0 && lines[static_cast<size_t>(code)].back().bg == kConvCodeBand && ConvRowWidth(lines[static_cast<size_t>(code)]) == cols,
              "layout: a code block keeps its spaces, on the code band across the row");
        const int typed = ConvFindRow(sheet, L"typed while it worked (10:30)");
        CHECK(typed > 0 && (lines[static_cast<size_t>(typed)][0].flags & kScreenCellItalic) != 0 && lines[static_cast<size_t>(typed)][0].fg == kConvDim,
              "layout: a label that says more than User stays, dim italic");
        // A turn with no tool call: the turns view's "(no tool calls)" note is left out, and ONE blank row separates the
        // prompt's band from the reply, as on Claude Code's screen.
        const int bare = ConvFindRow(sheet, L"answered without a tool");
        CHECK(ConvFindRow(sheet, L"(no tool calls)") < 0, "layout: the turns view's no-tool-calls note draws nothing");
        CHECK(bare > 2 && lines[static_cast<size_t>(bare) - 1].empty() && ConvRowText(lines[static_cast<size_t>(bare) - 2]).find(L"nothing to run here") != std::wstring::npos,
              "layout: one blank row between the prompt and a reply that used no tool");

        // The cap keeps the END: the note on top, the last rows as they were.
        const int total = static_cast<int>(lines.size());
        const ConversationSheet capped = ConvLayout(doc, cols, 8);
        CHECK(capped.snapshot.rows == 8 && capped.droppedRows == total - 6 && !capped.snapshot.lines.empty() && !capped.snapshot.lines[0].empty() &&
                  capped.snapshot.lines[0][0].text == L"\x22EF",
              "layout: a capped sheet keeps its last rows, under a note of how many were left out");
        CHECK(capped.snapshot.rows == 8 && ConvRowText(capped.snapshot.lines.back()) == ConvRowText(lines.back()) && capped.droppedThroughTurn >= 1,
              "layout: the capped sheet ends where the whole one ends, and names the last turn it cut");
        const ConversationSheet empty = ConvLayout(L"", cols);
        CHECK(empty.snapshot.rows == 1 && empty.snapshot.lines.size() == 1, "layout: an empty document is one blank row");
    }

    // ===== 3c. the layout of a ```diff block ("File diffs"): numbers, bands, hunks, a git header, notes, wrapping =====
    {
        // A call's change as TranscriptTurns' editDiffs writes it (two hunks; the second's numbers need 4 digits), then a reply
        // quoting a git diff (its file header before the hunk, a long added line), then a diff without hunk headers (the
        // turns view's old_string / new_string fallback) that removes a SQL comment, with a "\ No newline" note.
        const std::wstring doc =
            L"### Turn 1 \x00B7 now\n\n"
            L"**\x276F User**\n> fix it\n\n"
            L"**\x25CF Claude**\n"
            L"- **Edit** `C:/w/app.py` \x00B7 ok\n"
            L"  - ```diff\n"
            L"    @@ -3,2 +3,3 @@\n"
            L"     import sys\n"
            L"    -print('hi')\n"
            L"    +print('hello')\n"
            L"    +print('bye')\n"
            L"    @@ -1040,1 +1041,1 @@\n"
            L"    -x = 1\n"
            L"    +x = 2\n"
            L"    ```\n"
            L"- **Edit** `C:/w/q.sql` \x00B7 ok\n"
            L"  - ```diff\n"
            L"    --- old comment\n"
            L"    +-- new comment\n"
            L"    \\ No newline at end of file\n"
            L"    ```\n\n"
            L"> fixed\n"
            L">\n"
            L"> ```diff\n"
            L"> diff --git a/x.sql b/x.sql\n"
            L"> index 1234567..89abcde 100644\n"
            L"> --- a/x.sql\n"
            L"> +++ b/x.sql\n"
            L"> @@ -1 +1 @@\n"
            L"> -old\n"
            L"> +new line that is long enough to wrap past the forty columns\n"
            L"> ```\n";
        const int cols = 40;
        const ConversationSheet sheet = ConvLayout(doc, cols);
        const auto& lines = sheet.snapshot.lines;
        bool fits = true;
        for (const auto& row : lines)
        {
            fits = fits && ConvRowWidth(row) <= cols;
        }
        CHECK(!lines.empty() && fits, "diff: every row of a sheet with diffs stays within its columns");
        const auto rowAt = [&lines](int r) -> const std::vector<ScreenCell>& { return lines[static_cast<size_t>(r)]; };

        // The first hunk: the header draws nothing; a context row is plain, numbered in the new file, under the result corner.
        CHECK(ConvFindRow(sheet, L"@@") < 0, "diff: a hunk header draws no row");
        const int context = ConvFindRow(sheet, L"   3   import sys");
        CHECK(context > 0 && ConvCellIndexOf(rowAt(context), L"\x23BF") >= 0 && rowAt(context).back().bg == kConvBg,
              "diff: the block's first row hangs under the call's result corner; a context line is plain, numbered 3 (4-digit gutter)");
        const int number = context > 0 ? ConvCellIndexOf(rowAt(context), L"3   import") : -1;
        CHECK(number > 0 && rowAt(context)[static_cast<size_t>(number)].fg == kConvDim, "diff: the line number is dim");

        // A removed line: the old file's number, the red band from the number to the sheet's edge, the text bright.
        const int removed = ConvFindRow(sheet, L"   4 - print('hi')");
        const int removedText = removed > 0 ? ConvCellIndexOf(rowAt(removed), L"print('hi')") : -1;
        const int removedNumber = removed > 0 ? ConvCellIndexOf(rowAt(removed), L"   4 - ") : -1;
        CHECK(removed == context + 1 && removedText > 0 && rowAt(removed)[static_cast<size_t>(removedText)].bg == kConvDiffRemovedBg &&
                  rowAt(removed)[static_cast<size_t>(removedText)].fg == kConvBright && rowAt(removed).back().bg == kConvDiffRemovedBg &&
                  ConvRowWidth(rowAt(removed)) == cols,
              "diff: a removed line sits on the red band across the row, its text bright, numbered in the old file (4)");
        CHECK(removedNumber > 0 && rowAt(removed)[static_cast<size_t>(removedNumber)].bg == kConvDiffRemovedBg && rowAt(removed)[0].bg == kConvBg &&
                  ConvCellIndexOf(rowAt(removed), L"\x23BF") < 0,
              "diff: the band starts at the gutter (the containers' cells keep the sheet's color), and only the block's first row has the corner");

        // Added lines: the new file's numbers (4, 5), the green band.
        const int added = ConvFindRow(sheet, L"   4 + print('hello')");
        const int added2 = ConvFindRow(sheet, L"   5 + print('bye')");
        CHECK(added == removed + 1 && added2 == added + 1 && rowAt(added).back().bg == kConvDiffAddedBg && rowAt(added2).back().bg == kConvDiffAddedBg,
              "diff: added lines sit on the green band, numbered in the new file (4, 5)");

        // The second hunk: announced by a dim vertical ellipsis, numbered from its own header.
        const int gap = ConvFindRow(sheet, L"\x22EE");
        const int second = ConvFindRow(sheet, L"1040 - x = 1");
        CHECK(gap == added2 + 1 && second == gap + 1 && ConvCellIndexOf(rowAt(gap), L"\x22EE") >= 0 &&
                  rowAt(gap)[static_cast<size_t>(ConvCellIndexOf(rowAt(gap), L"\x22EE"))].fg == kConvDim && ConvFindRow(sheet, L"1041 + x = 2") == second + 1,
              "diff: a second hunk follows a dim vertical ellipsis and counts from its own header (1040 / 1041)");

        // Without hunk headers: no numbers; a lone "--- " line is a REMOVED "-- " line (not a file header); a note is dim italic.
        const int comment = ConvFindRow(sheet, L"    - -- old comment");
        const int commentText = comment > 0 ? ConvCellIndexOf(rowAt(comment), L"-- old comment") : -1;
        CHECK(commentText > 0 && rowAt(comment)[static_cast<size_t>(commentText)].bg == kConvDiffRemovedBg && rowAt(comment)[static_cast<size_t>(commentText)].fg == kConvBright,
              "diff: without hunk headers a line is drawn unnumbered, and a lone --- line is a removed -- line, not a header");
        const int newComment = ConvFindRow(sheet, L"    + -- new comment");
        CHECK(newComment == comment + 1 && rowAt(newComment).back().bg == kConvDiffAddedBg, "diff: the added comment follows, unnumbered, on the green band");
        const int note = ConvFindRow(sheet, L"No newline at end of file");
        const int noteText = note > 0 ? ConvCellIndexOf(rowAt(note), L"\\ No newline") : -1;
        CHECK(note == newComment + 1 && noteText > 0 && rowAt(note)[static_cast<size_t>(noteText)].fg == kConvDim &&
                  (rowAt(note)[static_cast<size_t>(noteText)].flags & kScreenCellItalic) != 0 && rowAt(note).back().bg == kConvBg,
              "diff: a \\ No newline note is dim italic, on no band");

        // A quoted git diff: its file header is dim and unnumbered, the hunk counts from 1 (a 3-digit gutter at least).
        const int header = ConvFindRow(sheet, L"diff --git a/x.sql b/x.sql");
        const int headerText = header > 0 ? ConvCellIndexOf(rowAt(header), L"diff --git") : -1;
        CHECK(headerText > 0 && rowAt(header)[static_cast<size_t>(headerText)].fg == kConvDim && rowAt(header).back().bg == kConvBg &&
                  ConvFindRow(sheet, L"index 1234567") == header + 1 && ConvFindRow(sheet, L"--- a/x.sql") == header + 2 && ConvFindRow(sheet, L"+++ b/x.sql") == header + 3,
              "diff: a quoted git diff's file header (diff, index, the ---/+++ pair) is dim and on no band");
        const int old = ConvFindRow(sheet, L"  1 - old");
        CHECK(old == header + 4 && rowAt(old).back().bg == kConvDiffRemovedBg, "diff: the quoted diff's hunk starts at line 1 (a 3-digit gutter)");

        // A long line breaks at the edge; the next row keeps the band and a blank gutter, so the text lines up under itself.
        const int longLine = ConvFindRow(sheet, L"  1 + new line that is");
        CHECK(longLine == old + 1 && static_cast<size_t>(longLine + 1) < lines.size() && rowAt(longLine + 1).back().bg == kConvDiffAddedBg &&
                  ConvRowWidth(rowAt(longLine + 1)) == cols && ConvFindRow(sheet, L"columns") > longLine,
              "diff: a line longer than the sheet breaks at the edge and goes on, on its band, under its text");
        const int wrapText = longLine > 0 ? ConvCellIndexOf(rowAt(longLine), L"new line") : -1;
        const int wrapGutter = longLine > 0 && static_cast<size_t>(longLine + 1) < lines.size() && wrapText > 0 ? wrapText - 1 : -1;
        CHECK(wrapGutter > 0 && static_cast<size_t>(wrapGutter) < rowAt(longLine + 1).size() && rowAt(longLine + 1)[static_cast<size_t>(wrapGutter)].text == L" " &&
                  rowAt(longLine + 1)[static_cast<size_t>(wrapGutter)].bg == kConvDiffAddedBg && rowAt(longLine + 1)[static_cast<size_t>(wrapText)].text != L" ",
              "diff: the continuation row's gutter is blank on the band, its text starting in the text's column");
    }

    // ===== 4. end to end: a turns fixture transcript drawn as pictures =====
    {
        const fs::path transcript = ConvHarnessDir() / L"turns" / L"fixture_session.jsonl";
        ConversationImageOptions options;
        options.utc = true;
        size_t parts = 0;
        std::string first;
        options.onPart = [&](ConversationImagePart&& part) {
            if (parts++ == 0)
            {
                first = std::move(part.picture.png);
            }
        };
        const ConversationImageResult r = RenderConversationImages(transcript, options);
        CHECK(r.error.empty() && parts >= 1 && r.parts.size() == parts && r.turns >= 1 && r.rows > 0, "end to end: the fixture session is drawn without an error");
        if (!r.error.empty())
        {
            std::wprintf(L"    conversation error: %ls\n", r.error.c_str());
        }
        ConvDecoded d;
        CHECK(ConvDecodePng(first, d) && d.width == static_cast<UINT>(kConversationImageCols * 12 + 32), "end to end: its first picture is a PNG of 100 cells of 12 px plus the padding");
        std::wprintf(L"    fixture_session: %d rows, %d turn(s), %zu part(s), %zu chars of turns view, %ls\n", r.rows, r.turns, parts, r.markdownChars, r.fontFamily.c_str());
        // "File diffs" on the same session: its one file change (a Write that created scan.py, two lines) becomes a diff block,
        // drawn as exactly two more rows (the hunk header draws none), and nothing else changes size.
        ConversationImageOptions withDiffs;
        withDiffs.utc = true;
        withDiffs.editDiffs = true;
        const ConversationImageResult diffs = RenderConversationImages(transcript, withDiffs);
        CHECK(diffs.error.empty() && diffs.rows == r.rows + 2 && diffs.markdownChars > r.markdownChars && diffs.turns == r.turns,
              "end to end: File diffs draws the session's created file as two more rows (its added lines)");
        if (diffs.rows != r.rows + 2)
        {
            std::wprintf(L"    with File diffs: %d rows (without: %d), error [%ls]\n", diffs.rows, r.rows, diffs.error.c_str());
        }
        const ConversationImageResult missing = RenderConversationImages(ConvHarnessDir() / L"turns" / L"no-such-transcript.jsonl", ConversationImageOptions{});
        CHECK(missing.parts.empty() && !missing.error.empty(), "end to end: a transcript that is not there is refused with a reason");
    }
#else
    std::wprintf(L"  (layout and end-to-end cases skipped: they need the vcpkg cmark of a full build - AM_HAVE_CMARK)\n");
#endif
}
