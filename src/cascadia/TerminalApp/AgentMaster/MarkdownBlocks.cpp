// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — MarkdownBlocks: cmark's document tree walked once, top to bottom, and flattened into
// MdBlock records (see MarkdownBlocks.h for what a block carries and why the list is flat).
//
// The walk is cmark's own iterator (ENTER / EXIT events; leaf nodes get a single ENTER), so it is
// non-recursive and its stack depth is independent of how deeply a document nests. Container state
// lives on two explicit stacks: `_stack` (the quotes and list items around the current position —
// copied into every block) and `_lists` (per open list: its numbering, tightness and marker width).

#ifndef NOMINMAX
#define NOMINMAX // <windows.h> below must not define min/max macros over std::min / std::max
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "MarkdownBlocks.h"

#include "ClaudeSpawn.h" // LogSwallowedException — Rule #18: a swallowed exception still reaches hooks.log

#include <windows.h> // WideCharToMultiByte / MultiByteToWideChar

// cmark's header defines short legacy aliases (NODE_TEXT, BULLET_LIST, ...) unless told not to; they
// are plain macros and would rewrite any identifier of the same name in this TU.
#define CMARK_NO_SHORT_NAMES
#include <cmark.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <cwctype> // iswspace
#include <memory>
#include <stdexcept> // std::length_error (ToUtf8's size refusal)

namespace Agentmaster
{
    namespace
    {
        // The vertical gaps MdGapAbove chooses between, in DIPs. Small on purpose: a transcript is
        // long, and the overlay is 80% of the window at most — every pixel of air costs content.
        constexpr double kGapParagraph = 8.0; // between ordinary paragraphs / quotes (CommonMark's paragraph spacing)
        constexpr double kGapTight = 3.0; // between the items of a tight list (transcript_turns' tool-call lines)
        constexpr double kGapTightCode = 4.0; // a code block inside / next to a tight list item (a hair more than text)
        constexpr double kGapAroundCode = 6.0; // a code block or rule next to ordinary text
        constexpr double kGapAfterLabel = 4.0; // the block right under a "**❯ User**" / "**● Claude**" label line
        constexpr double kGapHeadingMajor = 22.0; // above an h1 / h2 — a section break
        constexpr double kGapHeadingMinor = 16.0; // above an h3..h6 (transcript_turns' "### Turn N" headers)
        constexpr double kGapAfterHeading = 6.0; // the block right under a heading belongs to it

        // The longest label line MdIsLabelParagraph accepts: speaker lines are a few words, while a
        // long all-bold paragraph is emphasis, not a label, and must keep paragraph spacing after it.
        constexpr size_t kLabelMaxChars = 200;

        // The largest document ToUtf8 converts. WideCharToMultiByte takes int lengths; a transcript
        // anywhere near this (hundreds of MB of Markdown) is far past anything the overlay renders —
        // it caps its input long before — so refusing is the honest answer, not a silent wrap.
        constexpr size_t kMaxConvertibleChars = static_cast<size_t>(INT_MAX) / 4;

        // Frees a cmark node tree on scope exit — every exit, including an exception mid-walk.
        struct CmarkNodeDeleter
        {
            void operator()(cmark_node* node) const noexcept
            {
                cmark_node_free(node);
            }
        };

        // Frees a cmark iterator on scope exit (it must die before the tree it walks — declared
        // after the tree's owner, so destruction order guarantees that).
        struct CmarkIterDeleter
        {
            void operator()(cmark_iter* iter) const noexcept
            {
                cmark_iter_free(iter);
            }
        };

        // UTF-16 -> UTF-8 for cmark. An unpaired surrogate becomes U+FFFD (WideCharToMultiByte's
        // default replacement), which is what a renderer would show for it anyway.
        // Parameters: text — the whole document.
        // Returns the UTF-8 bytes; empty for empty input.
        // Throws std::length_error for a document past kMaxConvertibleChars (the int-length API
        // limit) and std::bad_alloc when the buffer can't be allocated — both reach the caller's
        // catch, which logs them and reports the parse as failed.
        std::string ToUtf8(std::wstring_view text)
        {
            if (text.empty())
            {
                return {};
            }
            if (text.size() > kMaxConvertibleChars)
            {
                throw std::length_error("MarkdownBlocks: document too large to convert");
            }
            const int wideLen = static_cast<int>(text.size());
            const int needed = WideCharToMultiByte(CP_UTF8, 0, text.data(), wideLen, nullptr, 0, nullptr, nullptr);
            if (needed <= 0)
            {
                return {};
            }
            std::string out(static_cast<size_t>(needed), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text.data(), wideLen, out.data(), needed, nullptr, nullptr);
            return out;
        }

        // UTF-8 (a cmark literal / URL / info string) -> UTF-16. cmark validated the input
        // (CMARK_OPT_VALIDATE_UTF8), so its strings are well-formed.
        // Parameters: text — a NUL-terminated cmark string, or nullptr (cmark answers nullptr for a
        //             field the node type doesn't have).
        // Returns the converted text; empty for nullptr / "".
        // Throws std::bad_alloc only.
        std::wstring FromUtf8(const char* text)
        {
            if (!text || !*text)
            {
                return {};
            }
            const size_t len = std::strlen(text);
            if (len > static_cast<size_t>(INT_MAX))
            {
                return {}; // a single cmark literal this size cannot come from a document ToUtf8 accepted
            }
            const int narrowLen = static_cast<int>(len);
            const int needed = MultiByteToWideChar(CP_UTF8, 0, text, narrowLen, nullptr, 0);
            if (needed <= 0)
            {
                return {};
            }
            std::wstring out(static_cast<size_t>(needed), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text, narrowLen, out.data(), needed);
            return out;
        }

        // The bullet glyph for an unordered list nested `depth` lists deep (0 == outermost):
        // • then ◦ then ▪ for everything deeper — the same progression WT's Markdown pane uses, so a
        // nested list reads the same in both.
        // Returns the glyph (no trailing space — the renderer's marker column provides the gap).
        std::wstring BulletFor(size_t depth)
        {
            switch (depth)
            {
            case 0:
                return L"\x2022"; // •
            case 1:
                return L"\x25E6"; // ◦
            default:
                return L"\x25AA"; // ▪
            }
        }

        // How many decimal digits `value` prints with (0 prints as "0" — one digit). Negative
        // values count their digits only; cmark never reports a negative list start.
        size_t DecimalDigits(long long value) noexcept
        {
            if (value < 0)
            {
                value = -value;
            }
            size_t digits = 1;
            while (value >= 10)
            {
                value /= 10;
                ++digits;
            }
            return digits;
        }

        // The first word of a fence's info string ("python title=x" -> "python"), capped at 32
        // characters — the language label, nothing more. Empty when the fence named nothing.
        std::wstring FirstWord(const std::wstring& info)
        {
            size_t start = 0;
            while (start < info.size() && iswspace(info[start]))
            {
                ++start;
            }
            size_t end = start;
            while (end < info.size() && !iswspace(info[end]) && end - start < 32)
            {
                ++end;
            }
            return info.substr(start, end - start);
        }

        // Cut `text` to at most `maxChars` UTF-16 units without splitting a surrogate pair (a lone
        // high surrogate at the end would draw as a replacement glyph).
        // Returns true when anything was cut.
        bool CutAt(std::wstring& text, size_t maxChars)
        {
            if (maxChars == 0 || text.size() <= maxChars)
            {
                return false;
            }
            size_t keep = maxChars;
            if (keep > 0 && IS_HIGH_SURROGATE(text[keep - 1]))
            {
                --keep;
            }
            text.resize(keep);
            return true;
        }

        // The walker: one instance per parse, fed every iterator event in order. Not thread-safe
        // (one parse == one thread), never shared.
        class BlockWalker
        {
        public:
            // Parameters:
            //   options — the caps and the soft-break rule (borrowed: must outlive the walker).
            //   out     — where blocks are appended (borrowed: the caller's result).
            BlockWalker(const MdParseOptions& options, MdParseResult& out) noexcept :
                _options{ options },
                _out{ out }
            {
            }

            // Handle one iterator event.
            // Parameters:
            //   node — the node the event is for (never null — cmark's iterator never yields one).
            //   ev   — CMARK_EVENT_ENTER or CMARK_EVENT_EXIT (leaf nodes only ever ENTER).
            // Returns false when the walk must STOP: MdParseOptions::maxBlocks was reached and the
            // block cap flag is set on the result. True to keep walking.
            // Throws std::bad_alloc (string / vector growth) — the caller logs it and fails the parse.
            bool Visit(cmark_node* node, cmark_event_type ev)
            {
                const bool entering = ev == CMARK_EVENT_ENTER;
                const auto type = cmark_node_get_type(node);
                switch (type)
                {
                case CMARK_NODE_BLOCK_QUOTE:
                    if (entering)
                    {
                        MdContainer quote;
                        quote.kind = MdContainerKind::Quote;
                        quote.id = _nextId++;
                        _stack.push_back(std::move(quote));
                    }
                    else if (!_stack.empty())
                    {
                        _stack.pop_back();
                    }
                    break;

                case CMARK_NODE_LIST:
                    if (entering)
                    {
                        _lists.push_back(_MakeListFrame(node));
                    }
                    else if (!_lists.empty())
                    {
                        _lists.pop_back();
                    }
                    break;

                case CMARK_NODE_ITEM:
                    if (entering)
                    {
                        _stack.push_back(_MakeItem());
                    }
                    else
                    {
                        // An item whose marker is STILL pending produced no block at all (a bare "-"
                        // line). Give it an empty paragraph, or its bullet / number would vanish and the
                        // list would read as if the item never existed.
                        if (!_stack.empty() && _stack.back().kind == MdContainerKind::Item && !_stack.back().marker.empty())
                        {
                            if (!_Begin(MdBlockKind::Paragraph))
                            {
                                return false;
                            }
                        }
                        if (!_stack.empty())
                        {
                            _stack.pop_back();
                        }
                    }
                    break;

                case CMARK_NODE_PARAGRAPH:
                case CMARK_NODE_HEADING:
                    if (entering)
                    {
                        const bool heading = type == CMARK_NODE_HEADING;
                        auto* block = _Begin(heading ? MdBlockKind::Heading : MdBlockKind::Paragraph);
                        if (!block)
                        {
                            return false;
                        }
                        if (heading)
                        {
                            block->headingLevel = static_cast<uint8_t>(std::clamp(cmark_node_get_heading_level(node), 1, 6));
                        }
                        _inInline = true; // the TEXT / CODE / ... children that follow land in this block
                    }
                    else
                    {
                        _EndInlineBlock();
                    }
                    break;

                case CMARK_NODE_CODE_BLOCK:
                {
                    auto* block = _Begin(MdBlockKind::Code);
                    if (!block)
                    {
                        return false;
                    }
                    block->code = FromUtf8(cmark_node_get_literal(node));
                    // cmark keeps the code block's final newline; drawn, it would be an empty last line.
                    if (!block->code.empty() && block->code.back() == L'\n')
                    {
                        block->code.pop_back();
                    }
                    block->truncated = CutAt(block->code, _options.maxBlockChars);
                    block->info = FirstWord(FromUtf8(cmark_node_get_fence_info(node)));
                    break;
                }

                case CMARK_NODE_HTML_BLOCK:
                {
                    // Kept as LITERAL text (see MarkdownBlocks.h): a raw HTML block in a transcript is
                    // something a message contained, and it must read exactly as written.
                    auto* block = _Begin(MdBlockKind::Paragraph);
                    if (!block)
                    {
                        return false;
                    }
                    block->rawHtml = true;
                    std::wstring text = FromUtf8(cmark_node_get_literal(node));
                    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r'))
                    {
                        text.pop_back();
                    }
                    block->truncated = CutAt(text, _options.maxBlockChars);
                    if (!text.empty())
                    {
                        MdRun run;
                        run.text = std::move(text);
                        run.style = kMdCode;
                        block->runs.push_back(std::move(run));
                    }
                    break;
                }

                case CMARK_NODE_THEMATIC_BREAK:
                    if (!_Begin(MdBlockKind::Rule))
                    {
                        return false;
                    }
                    break;

                case CMARK_NODE_TEXT:
                    _Append(FromUtf8(cmark_node_get_literal(node)), 0);
                    break;

                case CMARK_NODE_SOFTBREAK:
                    _Append(_options.softBreaksAsLineBreaks ? std::wstring_view{ L"\n" } : std::wstring_view{ L" " }, 0);
                    break;

                case CMARK_NODE_LINEBREAK:
                    _Append(L"\n", 0);
                    break;

                case CMARK_NODE_CODE:
                    _Append(FromUtf8(cmark_node_get_literal(node)), kMdCode);
                    break;

                case CMARK_NODE_HTML_INLINE:
                    // Literal and PLAIN: "use List<int> here" must read as the sentence the user wrote,
                    // not as monospace markup in the middle of prose.
                    _Append(FromUtf8(cmark_node_get_literal(node)), 0);
                    break;

                case CMARK_NODE_EMPH:
                    if (entering)
                    {
                        ++_emph;
                    }
                    else if (_emph > 0)
                    {
                        --_emph;
                    }
                    break;

                case CMARK_NODE_STRONG:
                    if (entering)
                    {
                        ++_strong;
                    }
                    else if (_strong > 0)
                    {
                        --_strong;
                    }
                    break;

                case CMARK_NODE_LINK:
                    if (entering)
                    {
                        _links.push_back(FromUtf8(cmark_node_get_url(node)));
                    }
                    else if (!_links.empty())
                    {
                        _links.pop_back();
                    }
                    break;

                case CMARK_NODE_IMAGE:
                    // Never fetched (MarkdownBlocks.h): "[image: <alt>]", the whole thing linked to the
                    // image's URL so it can still be opened deliberately. The alt text arrives as this
                    // node's TEXT children between the two events.
                    if (entering)
                    {
                        _links.push_back(FromUtf8(cmark_node_get_url(node)));
                        _Append(L"[image: ", 0);
                    }
                    else
                    {
                        _Append(L"]", 0);
                        if (!_links.empty())
                        {
                            _links.pop_back();
                        }
                    }
                    break;

                default:
                    // DOCUMENT is the root; CUSTOM_BLOCK / CUSTOM_INLINE are never produced by the parser.
                    break;
                }
                return true;
            }

        private:
            // Per open list: what its items need to know when they open.
            struct ListFrame
            {
                uint32_t id = 0; // the list's own container-namespace id (MdContainer::listId of its items)
                bool ordered = false; // numbered list
                bool paren = false; // "1)" delimiter instead of "1."
                bool tight = false; // no blank lines between items
                int next = 1; // the number the next ordered item shows
                uint8_t markerChars = 1; // the widest marker any of its items shows, in characters
            };

            // Read a LIST node's numbering + tightness and size its marker column. Counting the items
            // up front is what makes "9." and "10." line up: every item of the list indents by the
            // width of its WIDEST number, not its own.
            ListFrame _MakeListFrame(cmark_node* list)
            {
                ListFrame frame;
                frame.id = _nextId++;
                frame.ordered = cmark_node_get_list_type(list) == CMARK_ORDERED_LIST;
                frame.paren = cmark_node_get_list_delim(list) == CMARK_PAREN_DELIM;
                frame.tight = cmark_node_get_list_tight(list) != 0;
                frame.next = cmark_node_get_list_start(list);
                if (frame.ordered)
                {
                    int items = 0;
                    for (auto* child = cmark_node_first_child(list); child; child = cmark_node_next(child))
                    {
                        ++items;
                    }
                    const long long highest = static_cast<long long>(frame.next) + std::max(items, 1) - 1;
                    // +1 for the delimiter; capped so a pathological start number can't size a huge column.
                    frame.markerChars = static_cast<uint8_t>(std::min<size_t>(DecimalDigits(highest) + 1, 12));
                }
                return frame;
            }

            // A new list item container: its list's facts + the marker its FIRST block will show.
            MdContainer _MakeItem()
            {
                MdContainer item;
                item.kind = MdContainerKind::Item;
                item.id = _nextId++;
                if (_lists.empty())
                {
                    item.marker = BulletFor(0); // an item outside a list: cmark never builds one — belt
                    return item;
                }
                auto& frame = _lists.back();
                item.listId = frame.id;
                item.tightList = frame.tight;
                item.ordered = frame.ordered;
                item.markerChars = frame.markerChars;
                if (frame.ordered)
                {
                    item.marker = std::to_wstring(frame.next) + (frame.paren ? L")" : L".");
                    if (frame.next < INT_MAX)
                    {
                        ++frame.next;
                    }
                }
                else
                {
                    // The bullet glyph follows the LIST nesting depth (ordered parents count too), so a
                    // bullet list inside a numbered one still steps to ◦.
                    item.marker = BulletFor(_lists.size() - 1);
                }
                return item;
            }

            // Open a new block of `kind` with the current containers. Consumes every pending item
            // marker: the first block of an item shows its marker, every later one gets a blank
            // hanging indent.
            // Returns the new block, or nullptr when MdParseOptions::maxBlocks is reached (the walk
            // must stop; the result's truncatedBlocks is set).
            MdBlock* _Begin(MdBlockKind kind)
            {
                _EndInlineBlock(); // cmark never nests blocks inside an open paragraph — a belt, not a path
                if (_options.maxBlocks != 0 && _out.blocks.size() >= _options.maxBlocks)
                {
                    _out.truncatedBlocks = true;
                    return nullptr;
                }
                MdBlock block;
                block.kind = kind;
                block.containers = _stack;
                for (auto& container : _stack)
                {
                    container.marker.clear();
                }
                _out.blocks.push_back(std::move(block));
                _blockChars = 0;
                return &_out.blocks.back();
            }

            // Append inline text to the open Paragraph / Heading, with the current emphasis, strong and
            // link state OR'ed onto `extraStyle`. Merges into the previous run when the style and URL
            // match. Honors MdParseOptions::maxBlockChars (cuts + flags the block once). A no-op when
            // no inline block is open (inline content outside a paragraph never happens in CommonMark).
            void _Append(std::wstring_view text, uint8_t extraStyle)
            {
                if (!_inInline || _out.blocks.empty() || text.empty())
                {
                    return;
                }
                auto& block = _out.blocks.back();
                std::wstring piece{ text };
                if (_options.maxBlockChars != 0)
                {
                    if (_blockChars >= _options.maxBlockChars)
                    {
                        block.truncated = true;
                        return;
                    }
                    if (CutAt(piece, _options.maxBlockChars - _blockChars))
                    {
                        block.truncated = true;
                    }
                    if (piece.empty())
                    {
                        return;
                    }
                }
                uint8_t style = extraStyle;
                if (_strong > 0)
                {
                    style |= kMdBold;
                }
                if (_emph > 0)
                {
                    style |= kMdItalic;
                }
                const std::wstring* url = nullptr;
                if (!_links.empty())
                {
                    style |= kMdLink;
                    url = &_links.back();
                }
                _blockChars += piece.size();
                if (!block.runs.empty())
                {
                    auto& last = block.runs.back();
                    const bool sameUrl = url ? last.url == *url : last.url.empty();
                    if (last.style == style && sameUrl)
                    {
                        last.text.append(piece);
                        return;
                    }
                }
                MdRun run;
                run.text = std::move(piece);
                run.style = style;
                if (url)
                {
                    run.url = *url;
                }
                block.runs.push_back(std::move(run));
            }

            // Close the open Paragraph / Heading: drop a trailing line break (it would draw an empty
            // last line) and any run it leaves empty. Idempotent.
            void _EndInlineBlock() noexcept
            {
                if (!_inInline)
                {
                    return;
                }
                _inInline = false;
                if (_out.blocks.empty())
                {
                    return;
                }
                auto& runs = _out.blocks.back().runs;
                while (!runs.empty())
                {
                    auto& text = runs.back().text;
                    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r'))
                    {
                        text.pop_back();
                    }
                    if (!text.empty())
                    {
                        break;
                    }
                    runs.pop_back();
                }
            }

            const MdParseOptions& _options;
            MdParseResult& _out;
            std::vector<MdContainer> _stack; // the quotes / items around the current position, outermost first
            std::vector<ListFrame> _lists; // the open lists, outermost first
            std::vector<std::wstring> _links; // the open links' / images' URLs, innermost last
            uint32_t _nextId = 1; // the next container / list id (0 is never a real one)
            int _strong = 0; // open **strong** nodes
            int _emph = 0; // open *emphasis* nodes
            bool _inInline = false; // a Paragraph / Heading is open: inline events append to _out.blocks.back()
            size_t _blockChars = 0; // UTF-16 units appended to the open block so far (maxBlockChars)
        };
    }

    MdParseResult ParseMarkdownBlocks(std::wstring_view markdown, const MdParseOptions& options) noexcept
    {
        MdParseResult result;
        try
        {
            const std::string utf8 = ToUtf8(markdown);
            if (utf8.empty())
            {
                return result;
            }
            // VALIDATE_UTF8: a belt — ToUtf8's output is well-formed, but cmark then guarantees every
            // literal it hands back is too, which FromUtf8 relies on. No SMART (it would rewrite the
            // quotes and dashes a transcript quotes verbatim), no HARDBREAKS (soft breaks are decided
            // by MdParseOptions, per event).
            std::unique_ptr<cmark_node, CmarkNodeDeleter> document{ cmark_parse_document(utf8.data(), utf8.size(), CMARK_OPT_DEFAULT | CMARK_OPT_VALIDATE_UTF8) };
            if (!document)
            {
                result.failed = true;
                AppendStateLog(L"hooks.log", L"[transcript] markdown parse failed: cmark returned no document (chars=" + std::to_wstring(markdown.size()) + L")\n");
                return result;
            }
            // Joins the adjacent TEXT nodes cmark splits at '[', '!', '<', ... so a paragraph is a few
            // runs instead of one per punctuation-delimited fragment.
            cmark_consolidate_text_nodes(document.get());
            std::unique_ptr<cmark_iter, CmarkIterDeleter> iter{ cmark_iter_new(document.get()) };
            if (!iter)
            {
                result.failed = true;
                AppendStateLog(L"hooks.log", L"[transcript] markdown parse failed: cmark returned no iterator\n");
                return result;
            }
            BlockWalker walker{ options, result };
            for (auto ev = cmark_iter_next(iter.get()); ev != CMARK_EVENT_DONE; ev = cmark_iter_next(iter.get()))
            {
                if (!walker.Visit(cmark_iter_get_node(iter.get()), ev))
                {
                    break; // the block cap was reached — what was parsed is the answer
                }
            }
        }
        catch (...)
        {
            // Rule #18. Recovery = the blocks parsed so far, flagged failed; the overlay shows them and
            // says the rest could not be rendered.
            result.failed = true;
            LogSwallowedException(L"ParseMarkdownBlocks");
        }
        return result;
    }

    size_t MdSharedContainerDepth(const MdBlock& prev, const MdBlock& cur) noexcept
    {
        const size_t limit = std::min(prev.containers.size(), cur.containers.size());
        size_t depth = 0;
        // Ids are unique per container node, so an equal id at the same position IS the same quote /
        // item — no kind check needed.
        while (depth < limit && prev.containers[depth].id == cur.containers[depth].id)
        {
            ++depth;
        }
        return depth;
    }

    bool MdIsLabelParagraph(const MdBlock& b) noexcept
    {
        if (b.kind != MdBlockKind::Paragraph || b.rawHtml || b.runs.empty())
        {
            return false;
        }
        // Only the FIRST LINE is judged. At the Full / Max levels transcript_turns writes the turn's stats
        // line right under "**● Claude**" with a single newline, and CommonMark folds the two into ONE
        // paragraph — the label is still the first line, whatever follows it.
        size_t chars = 0;
        bool sawBold = false;
        for (const auto& run : b.runs)
        {
            const std::wstring_view text{ run.text };
            const auto lineEnd = text.find_first_of(L"\r\n");
            const auto firstLinePart = lineEnd == std::wstring_view::npos ? text : text.substr(0, lineEnd);
            chars += firstLinePart.size();
            bool blank = true;
            for (const wchar_t ch : firstLinePart)
            {
                if (!iswspace(ch))
                {
                    blank = false;
                    break;
                }
            }
            if (!blank)
            {
                if ((run.style & kMdBold) == 0)
                {
                    return false; // plain text on the first line: prose with some bold in it, not a label
                }
                sawBold = true;
            }
            if (lineEnd != std::wstring_view::npos)
            {
                break; // the first line ended inside this run
            }
        }
        return sawBold && chars <= kLabelMaxChars;
    }

    double MdGapAbove(const MdBlock* prev, const MdBlock& cur) noexcept
    {
        if (!prev)
        {
            return 0.0;
        }
        if (cur.kind == MdBlockKind::Heading)
        {
            return cur.headingLevel <= 2 ? kGapHeadingMajor : kGapHeadingMinor;
        }
        if (prev->kind == MdBlockKind::Heading)
        {
            return kGapAfterHeading;
        }
        if (MdIsLabelParagraph(*prev))
        {
            return kGapAfterLabel;
        }
        const bool codeInvolved = cur.kind == MdBlockKind::Code || prev->kind == MdBlockKind::Code ||
                                  cur.kind == MdBlockKind::Rule || prev->kind == MdBlockKind::Rule;
        const size_t shared = MdSharedContainerDepth(*prev, cur);
        bool tight = false;
        if (shared < cur.containers.size())
        {
            // `cur` opens a container `prev` was not in. A new item of a TIGHT list is packed whether
            // it is the next sibling, the first item of a list nested in prev's item, or the start of a
            // list right under a paragraph; a quote (or a loose list) gets paragraph spacing.
            const auto& opened = cur.containers[shared];
            tight = opened.kind == MdContainerKind::Item && opened.tightList;
        }
        else if (!cur.containers.empty())
        {
            // Every container of `cur` was shared with `prev`: a continuation block (an item's second
            // paragraph or code block, or the text after a nested list). Packed when its innermost
            // container is an item of a tight list; a quote's paragraphs keep paragraph spacing.
            const auto& inner = cur.containers.back();
            tight = inner.kind == MdContainerKind::Item && inner.tightList;
        }
        if (tight)
        {
            return codeInvolved ? kGapTightCode : kGapTight;
        }
        return codeInvolved ? kGapAroundCode : kGapParagraph;
    }

    // ---- tool areas (MarkdownBlocks.h): what the transcript overlay folds -----------------------
    namespace
    {
        // The marks transcript_turns starts its speaker headers and list items with (TranscriptTurns.cpp
        // SpeakerBlock / EventLines). The two emoji are astral, so each is matched as its UTF-16
        // surrogate pair: one shared high half, a distinct low half.
        constexpr wchar_t kSpeakerMark = L'\x25CF'; // ● — a speaker header (Claude, an agent entry)
        constexpr wchar_t kPromptMark = L'\x276F'; // ❯ — a prompt / brief, or (nested) an answer
        constexpr wchar_t kMessageMark = L'\x2709'; // ✉ — a message that reached the agent
        constexpr wchar_t kAstralHigh = L'\xD83D'; // the high surrogate both emoji below share
        constexpr wchar_t kRemarkLow = L'\xDCAC'; // 💬 U+1F4AC — a remark between calls
        constexpr wchar_t kCompactionLow = L'\xDDDC'; // 🗜 U+1F5DC — a context compaction

        // The longest tool name a summary shows; a longer one (an MCP tool's qualified name) is cut
        // and ends in an ellipsis.
        constexpr size_t kToolNameMaxChars = 40;

        // A block's first VISIBLE UTF-16 unit: where an item's marker glyph (or its bold tool name) is.
        struct LeadUnit
        {
            wchar_t unit = L'\0'; // L'\0' == the block shows no text (an empty item, a code block)
            wchar_t next = L'\0'; // the unit right after it in the same run — a surrogate pair's low half
            uint8_t style = 0; // the kMd* bits of the run that holds it
            size_t run = 0; // that run's index in MdBlock::runs
        };

        // Scan `block`'s runs for the first non-blank unit. Never throws.
        LeadUnit FirstVisibleUnit(const MdBlock& block) noexcept
        {
            LeadUnit lead;
            for (size_t r = 0; r < block.runs.size(); ++r)
            {
                const std::wstring& text = block.runs[r].text;
                for (size_t i = 0; i < text.size(); ++i)
                {
                    if (iswspace(text[i]))
                    {
                        continue;
                    }
                    lead.unit = text[i];
                    lead.next = i + 1 < text.size() ? text[i + 1] : L'\0';
                    lead.style = block.runs[r].style;
                    lead.run = r;
                    return lead;
                }
            }
            return lead;
        }

        // A SPEAKER HEADER (MdFindToolAreas): top-level, not raw HTML, first visible unit '●' in a bold
        // run. Never throws.
        bool IsSpeakerHeader(const MdBlock& block) noexcept
        {
            if (block.kind != MdBlockKind::Paragraph || block.rawHtml || !block.containers.empty())
            {
                return false;
            }
            const LeadUnit lead = FirstVisibleUnit(block);
            return lead.unit == kSpeakerMark && (lead.style & kMdBold) != 0;
        }

        // `block` is the FIRST block of a top-level list item — the only block of the item that carries
        // its marker (MdContainer::marker). Never throws.
        bool StartsTopLevelItem(const MdBlock& block) noexcept
        {
            return !block.containers.empty() && block.containers[0].kind == MdContainerKind::Item && !block.containers[0].marker.empty();
        }

        // `block` is the first block of an item NESTED directly inside a top-level item (depth 2): the
        // shape of the answer / output / background lines transcript_turns puts under a call. Never throws.
        bool StartsNestedItem(const MdBlock& block) noexcept
        {
            return block.containers.size() >= 2 && block.containers[1].kind == MdContainerKind::Item && !block.containers[1].marker.empty();
        }

        // The block's whole text without surrounding blanks — for the "(no tool calls)" test only.
        // Throws std::bad_alloc only (the caller's catch logs it).
        std::wstring TrimmedText(const MdBlock& block)
        {
            std::wstring text;
            for (const auto& run : block.runs)
            {
                text += run.text;
            }
            const auto first = text.find_first_not_of(L" \t\r\n");
            if (first == std::wstring::npos)
            {
                return {};
            }
            const auto last = text.find_last_not_of(L" \t\r\n");
            return text.substr(first, last - first + 1);
        }

        // Count one top-level item (its first block) into `area` by its leading mark. A bold lead is a
        // tool call: OpSummary writes "**Name**" first, so the bold run's text IS the tool name.
        // Throws std::bad_alloc only (a new tool name entry).
        void CountTopLevelItem(const MdBlock& block, MdToolArea& area)
        {
            const LeadUnit lead = FirstVisibleUnit(block);
            if (lead.unit == kAstralHigh && lead.next == kRemarkLow)
            {
                ++area.remarks;
            }
            else if (lead.unit == kAstralHigh && lead.next == kCompactionLow)
            {
                ++area.compactions;
            }
            else if (lead.unit == kMessageMark)
            {
                ++area.messages;
            }
            else if (lead.unit == kPromptMark)
            {
                ++area.prompts;
            }
            else if (lead.unit != L'\0' && (lead.style & kMdBold) != 0)
            {
                ++area.toolCalls;
                std::wstring name = block.runs[lead.run].text;
                const auto from = name.find_first_not_of(L" \t\r\n");
                const auto to = name.find_last_not_of(L" \t\r\n");
                name = from == std::wstring::npos ? std::wstring{} : name.substr(from, to - from + 1);
                if (CutAt(name, kToolNameMaxChars - 1))
                {
                    name += L'\x2026';
                }
                for (auto& [known, calls] : area.tools)
                {
                    if (known == name)
                    {
                        ++calls;
                        return;
                    }
                }
                area.tools.emplace_back(std::move(name), size_t{ 1 });
            }
            else
            {
                ++area.others;
            }
        }

        // "1 remark" / "3 remarks" — the summary's count phrases. Throws std::bad_alloc only.
        std::wstring CountPhrase(size_t count, std::wstring_view singular, std::wstring_view plural)
        {
            return std::to_wstring(count) + L" " + std::wstring{ count == 1 ? singular : plural };
        }
    }

    std::vector<MdToolArea> MdFindToolAreas(const std::vector<MdBlock>& blocks) noexcept
    {
        std::vector<MdToolArea> areas;
        try
        {
            for (size_t i = 0; i + 1 < blocks.size(); ++i)
            {
                if (!IsSpeakerHeader(blocks[i]) || !StartsTopLevelItem(blocks[i + 1]))
                {
                    continue;
                }
                MdToolArea area;
                area.first = i + 1;
                // Every block of the list shares its OUTERMOST container's list id: the items themselves,
                // their continuation blocks, and the lists / code blocks nested inside them.
                const uint32_t listId = blocks[i + 1].containers[0].listId;
                size_t j = i + 1;
                for (; j < blocks.size(); ++j)
                {
                    const MdBlock& block = blocks[j];
                    if (block.containers.empty() || block.containers[0].kind != MdContainerKind::Item || block.containers[0].listId != listId)
                    {
                        break;
                    }
                    if (StartsTopLevelItem(block))
                    {
                        CountTopLevelItem(block, area);
                    }
                    else if (StartsNestedItem(block) && FirstVisibleUnit(block).unit == kPromptMark)
                    {
                        ++area.answers;
                    }
                }
                area.end = j;
                // A speaker with no call and no remark: transcript_turns writes the single item
                // "(no tool calls)" (or "(no remarks)") — nothing to fold.
                if (area.end - area.first == 1 && area.others == 1 && area.toolCalls == 0)
                {
                    const std::wstring text = TrimmedText(blocks[area.first]);
                    if (text == L"(no tool calls)" || text == L"(no remarks)")
                    {
                        area.empty = true;
                        area.others = 0;
                    }
                }
                areas.push_back(std::move(area));
                i = j - 1; // resume after the list (the loop's ++i lands on the block that ended it)
            }
        }
        catch (...)
        {
            // Rule #18. Recovery = the areas found so far: the overlay folds those and shows the rest as
            // plain lists, which is the pre-fold rendering — nothing is lost.
            LogSwallowedException(L"MdFindToolAreas");
        }
        return areas;
    }

    bool MdToolAreaFoldable(const MdToolArea& area) noexcept
    {
        return !area.empty && area.toolCalls + area.remarks > 0;
    }

    std::wstring MdToolAreaSummary(const MdToolArea& area, size_t maxTools) noexcept
    {
        try
        {
            std::vector<std::wstring> parts;
            if (area.toolCalls > 0)
            {
                std::wstring calls = CountPhrase(area.toolCalls, L"tool call", L"tool calls");
                if (maxTools > 0 && !area.tools.empty())
                {
                    // Most-called first; a STABLE sort keeps the first-call order among equal counts.
                    auto byCalls = area.tools;
                    std::stable_sort(byCalls.begin(), byCalls.end(), [](const auto& a, const auto& b) {
                        return a.second > b.second;
                    });
                    const size_t shown = std::min(maxTools, byCalls.size());
                    calls += L": ";
                    for (size_t k = 0; k < shown; ++k)
                    {
                        if (k > 0)
                        {
                            calls += L", ";
                        }
                        calls += byCalls[k].first;
                        if (byCalls[k].second > 1)
                        {
                            calls += L" \x00D7"; // the multiplication sign: "Bash ×3"
                            calls += std::to_wstring(byCalls[k].second);
                        }
                    }
                    if (byCalls.size() > shown)
                    {
                        calls += L" +" + std::to_wstring(byCalls.size() - shown) + L" more";
                    }
                }
                parts.push_back(std::move(calls));
            }
            if (area.remarks > 0)
            {
                parts.push_back(CountPhrase(area.remarks, L"remark", L"remarks"));
            }
            if (area.messages > 0)
            {
                parts.push_back(CountPhrase(area.messages, L"message", L"messages"));
            }
            if (area.prompts > 0)
            {
                parts.push_back(CountPhrase(area.prompts, L"prompt", L"prompts"));
            }
            if (area.answers > 0)
            {
                parts.push_back(CountPhrase(area.answers, L"answer", L"answers"));
            }
            if (area.compactions > 0)
            {
                parts.push_back(CountPhrase(area.compactions, L"compaction", L"compactions"));
            }
            if (area.others > 0)
            {
                parts.push_back(CountPhrase(area.others, L"other line", L"other lines"));
            }
            std::wstring summary;
            for (size_t k = 0; k < parts.size(); ++k)
            {
                if (k > 0)
                {
                    summary += L" \x00B7 "; // the middle dot transcript_turns separates its facts with
                }
                summary += parts[k];
            }
            return summary;
        }
        catch (...)
        {
            // Rule #18. Recovery = no summary text; the overlay then falls back to a generic label.
            LogSwallowedException(L"MdToolAreaSummary");
            return {};
        }
    }
}
