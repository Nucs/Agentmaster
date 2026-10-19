"""Agentmaster CPU hunt: what the UI thread was doing, from a stacksnap profile.

Why: `stacksnap sample` writes every sampled stack in folded form (stacksnap_<pid>_<tid>.folded). On the UI
thread the interesting split is not by leaf but by ACTIVITY: XAML's frame tick (its composition-tree walk,
its animation tick, its layout), our dispatcher callbacks named by their first TerminalApp lane, input, and
waiting. This prints that split as shares of the busy samples and, given the thread's ms/s from the
profile's header, as ms/s.

The walk's internals often stop the stack walk before it reaches the frame tick (dcomp's VisualIterator,
CoreMessaging's AllocateItemForPeer / RemoveItem, critical sections); those are counted as "composition
walk pieces" and most of the "walk stopped early" leaves belong to the walk too.

Usage: python -I uiclass.py <stacksnap_<pid>_<tid>.folded> [the thread's ms/s from the profile header]
"""

import collections
import re
import sys

# Our lane names: coroutine bodies (_XxxImpl$_ResumeCoro), private members (::_Xxx), handlers and ticks.
LANE = re.compile(r"(_Impl|Impl\$|_ResumeCoro|::_[A-Z]\w+|Handler|Tick|::On[A-Z])")
WALK = ("UpdateTree", "VisualIterator", "VisualCollection", "GetWUCVisual", "IExportMessageConversation", "ProcessDestroyWorkflow")
WAITS = ("NtWait", "ZwWait", "NtUser")


def classify(stack):
    """Name the activity of one folded stack.

    Args:
        stack: the stack, root first, frames joined by ';'.

    Returns:
        The activity label (frame tick parts, our lane, input, layout, waiting, or the leaf of a walk that
        stopped early).

    Raises:
        Nothing.
    """
    frames = stack.split(";")
    if "CXcpBrowserHost::OnTick" in stack:
        if "UpdateTree" in stack or "SubmitPrimitiveCompositionCommands" in stack or "SubmitRenderCommandsToCompositor" in stack:
            return "frame tick: composition tree walk"
        if "CTimeManager::Tick" in stack or "CCoreServices::Tick" in stack:
            return "frame tick: animation tick"
        if "UpdateLayout" in stack or "CLayoutManager" in stack:
            return "frame tick: layout"
        return "frame tick: other"
    if any(w in stack for w in WALK):
        return "composition walk pieces (the walk did not reach the tick)"
    mine = [f for f in frames if f.startswith(("TerminalApp!", "Microsoft.Terminal.", "TerminalControl!"))]
    for f in mine:
        if LANE.search(f) and "winrt::impl" not in f and "wil::details" not in f:
            return "our code: " + re.sub(r"\$_ResumeCoro\$\d+", "", f.split("!", 1)[1])[:110]
    if mine:
        return "our code (unnamed): " + mine[-1].split("!", 1)[1][:90]
    if "Input" in stack or "Pointer" in stack or "Keyboard" in stack:
        return "input"
    if "UpdateLayout" in stack or "CLayoutManager" in stack:
        return "layout outside the tick"
    if frames[-1].split("!")[-1].startswith(WAITS):
        return "waiting"
    return "other / walk stopped early: " + frames[-1][:80]


def main(argv):
    """Read the folded file and print the activity split.

    Args:
        argv: the folded file path, then optionally the thread's ms/s.

    Returns:
        0, or 2 on a usage error.

    Raises:
        OSError: when the file cannot be read.
    """
    if not argv:
        print(__doc__)
        return 2
    rate = float(argv[1]) if len(argv) > 1 else None
    buckets = collections.Counter()
    total = 0
    with open(argv[0], encoding="utf-8-sig") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line:
                continue
            stack, _, n = line.rpartition(" ")
            total += int(n)
            buckets[classify(stack)] += int(n)
    busy = total - buckets.get("waiting", 0)
    print(f"{total} samples, {busy} not waiting")
    agg = collections.Counter()
    for k, v in buckets.items():
        agg["other / walk stopped early (all leaves)" if k.startswith("other / walk stopped early") else k] += v
    for k, v in agg.most_common(40):
        if k != "waiting" and busy:
            ms = f"  ~{rate * v / busy:6.1f} ms/s" if rate else ""
            print(f"  {v:6}  {100 * v / busy:5.1f}%{ms}  {k}")
    print("\ntop leaves of the walks that stopped early:")
    others = [(k, v) for k, v in buckets.items() if k.startswith("other / walk stopped early")]
    for k, v in sorted(others, key=lambda kv: -kv[1])[:15]:
        print(f"  {v:6}  {k[len('other / walk stopped early: '):]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
