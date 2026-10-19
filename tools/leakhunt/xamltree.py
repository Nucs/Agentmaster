"""Agentmaster leak/CPU hunt: where XAML's composition nodes and measured TextBlocks sit in the element tree.

Why: every XAML frame walks every composition node of every window (doc/agentmaster/UI_THREAD.md §10h), so
the frame cost is the node count; and every measured TextBlock keeps ~17 KB of DirectWrite state
(MEMORY_LEAKS.md §4.1). Both are paid per element that EXISTS, on screen or not. This reads a full dump and
attributes each population to its place in the tree, so a list that keeps every item loaded (the side
panel's cards) shows as one big group.

Modes:
    nodes  every HWCompTreeNodeWinRT (a composition-tree node) -> its CUIElement -> the element's class
           chain up to the root; grouped by the chain up to the nearest ScrollViewer, plus tab items.
    text   every CTextBlock: inside a list card (a CBorder directly in a StackPanel that is a ScrollViewer's
           content) or elsewhere, and measured (it holds a DirectWrite layout) or not.

Layout facts, measured on Windows 11 26200's system Windows.UI.Xaml.dll (2026-10-20) and checked by the
tool on every run (the parent offset is voted from the data; a wrong one prints a low vote):
    HWCompTreeNodeWinRT +0x98  the owning CUIElement (359 of the first 400 nodes pointed at a CBorder)
    CDependencyObject  +0x38   the parent element (300 of 300 votes)
    CTextBlock         +0x1F8  its DWriteTextLayout (0 = never measured)

Inputs: the dump, its heapcensus TSV (vftable names), and segheap.py --blocks-out of the SAME dump made by the
corrected walker (2026-10-20: segheap.py before that date misread the LFH bitmap).

Usage: python -I xamltree.py <dump> <census.tsv> <blocks.tsv> nodes|text [--top 30]
"""

import argparse
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import segheap  # noqa: E402

NODE_ELEMENT_OFFSET = 0x98
TEXTBLOCK_LAYOUT_OFFSET = 0x1F8
# A list card: the card CBorder, then its parents up to the list's ScrollViewer.
CARD_PARENTS = ["CStackPanel", "CScrollContentPresenter", "CGrid", "CBorder", "CScrollViewer"]


class Tree:
    """Class names and parent links of the core XAML objects of one dump."""

    def __init__(self, dump, census, blocks):
        """Load the dump, its vftable names and its live block starts.

        Args:
            dump: the full-memory dump path.
            census: the heapcensus TSV of the same dump.
            blocks: segheap.py --blocks-out of the same dump.

        Raises:
            OSError / ValueError: on unreadable inputs.
        """
        self.d = segheap.Dump(dump)
        self.vt = segheap.load_vtables(self.d, census)
        self.starts = []
        with open(blocks, encoding="ascii") as f:
            for line in f:
                self.starts.append(int(line.split("\t", 1)[0], 16))
        self.parent_offset = None

    def cls(self, ptr):
        """Name the core class of the object at ptr from its vftable.

        Args:
            ptr: an object address.

        Returns:
            The class name (e.g. "CBorder"), or None for no known vftable.

        Raises:
            Nothing.
        """
        if not ptr or ptr & 7:
            return None
        hit = self.vt.get(self.d.q(ptr))
        if not hit:
            return None
        n = hit[1].split(" | ")[0]
        return n[6:].split("::`vftable'")[0] if n.startswith("const ") else n

    def objects_of(self, class_name):
        """List the live blocks whose first qword is the vftable of one class.

        Args:
            class_name: the exact core class name, e.g. "CTextBlock".

        Returns:
            The block addresses.

        Raises:
            StopIteration: when the census has no vftable of that class.
        """
        want = next(va for va, (_m, n) in self.vt.items() if n.split(" | ")[0] == f"const {class_name}::`vftable'")
        return [a for a in self.starts if self.d.q(a) == want]

    def vote_parent_offset(self, elements):
        """Find the parent-pointer offset: the qword in the first 0x80 bytes that most often points at a core element.

        Args:
            elements: element addresses to vote with (a few hundred are enough).

        Returns:
            (offset, votes of the winner, number of elements voted).

        Raises:
            Nothing.
        """
        votes = collections.Counter()
        for e in elements:
            raw = self.d.read(e, 0x80) or b""
            for off in range(8, len(raw) - 7, 8):
                c = self.cls(struct.unpack_from("<Q", raw, off)[0])
                if c and c.startswith("C") and c[1:2].isupper():
                    votes[off] += 1
        off, n = votes.most_common(1)[0]
        self.parent_offset = off
        return off, n, len(elements)

    def chain(self, element, limit=80):
        """The element's class chain and addresses up to the root.

        Args:
            element: the starting element address.
            limit: the most levels to follow (a cycle guard).

        Returns:
            (class names, addresses), the element first.

        Raises:
            Nothing.
        """
        names, ptrs, seen, p = [], [], set(), element
        while p and p not in seen and len(names) < limit:
            seen.add(p)
            names.append(self.cls(p) or "?")
            ptrs.append(p)
            p = self.d.q(p + self.parent_offset)
        return names, ptrs


def nodes_report(t, top):
    """Group the composition nodes by where their elements sit.

    Args:
        t: the Tree.
        top: how many groups to print.

    Returns:
        Nothing.

    Raises:
        StopIteration: when the dump's census has no HWCompTreeNodeWinRT vftable.
    """
    nodes = t.objects_of("HWCompTreeNodeWinRT")
    elems = [t.d.q(a + NODE_ELEMENT_OFFSET) for a in nodes]
    off, n, of = t.vote_parent_offset(elems[:300])
    print(f"{len(nodes)} composition nodes; parent offset +0x{off:X} ({n} of {of} votes)")
    owners = collections.Counter(t.cls(e) for e in elems)
    print("owning element classes:", owners.most_common(8))
    region = collections.Counter()
    for e in elems:
        names, _p = t.chain(e)
        if "CListViewItem" in names[1:]:
            region["tab strip / list item (CListViewItem)"] += 1
        elif "CScrollViewer" in names:
            k = names.index("CScrollViewer")
            region[f"under a ScrollViewer {k} levels up: " + " < ".join(names[:min(k + 1, 7)])] += 1
        else:
            region["not in a ScrollViewer: " + " < ".join(names[:6])] += 1
    for k, v in region.most_common(top):
        print(f"  {v:6}  {100 * v / len(elems):5.1f}%  {k[:180]}")


def text_report(t):
    """Count the CTextBlocks in list cards and elsewhere, measured or not.

    Args:
        t: the Tree.

    Returns:
        Nothing.

    Raises:
        StopIteration: when the dump's census has no CTextBlock vftable.
    """
    tbs = t.objects_of("CTextBlock")
    off, n, of = t.vote_parent_offset(tbs[:300])
    print(f"{len(tbs)} TextBlocks; parent offset +0x{off:X} ({n} of {of} votes)")
    stat = collections.Counter()
    cards = set()
    for a in tbs:
        names, ptrs = t.chain(a)
        card = None
        for i in range(1, len(names) - len(CARD_PARENTS)):
            if names[i] == "CBorder" and names[i + 1:i + 1 + len(CARD_PARENTS)] == CARD_PARENTS:
                card = ptrs[i]
                break
        measured = t.d.q(a + TEXTBLOCK_LAYOUT_OFFSET) not in (0, None)
        stat[("in a list card" if card else "elsewhere", "measured" if measured else "not measured")] += 1
        if card:
            cards.add(card)
    print(f"list cards holding them: {len(cards)}")
    for k, v in sorted(stat.items()):
        print(f"  {v:7}  {k}")
    m = stat[("in a list card", "measured")]
    print(f"measured TextBlocks in list cards: {m} = ~{m * 17 / 1024:.0f} MB of DirectWrite state at ~17 KB each")


def main():
    """Parse the arguments and print the chosen report.

    Returns:
        0.

    Raises:
        OSError / ValueError: on unreadable inputs.
    """
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("dump")
    ap.add_argument("census")
    ap.add_argument("blocks")
    ap.add_argument("mode", choices=("nodes", "text"))
    ap.add_argument("--top", type=int, default=30)
    a = ap.parse_args()
    t = Tree(a.dump, a.census, a.blocks)
    if a.mode == "nodes":
        nodes_report(t, a.top)
    else:
        text_report(t)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
