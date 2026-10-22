"""Live probe: user interrupts (Esc), the "uninterrupt" rewind line, and forks of a running turn.

This is the measurement behind doc/agentmaster/HOOKS.md "Interrupted turns" (2026-10-22): the Restart
question, the Fork question and the fork stop watch are all built on what these experiments showed on
Claude Code 2.1.291. Re-run them after a Claude Code update; a changed result means the planner, the
classifier or the docs must change too (README.md says where).

It drives the REAL claude.exe (the official installer's ~/.local/bin copy) on a ConPTY (the repo's
claude-wrap-ConPTY skill) against tools/e2e-spike/mockllm.py, so no request leaves 127.0.0.1, in an isolated
CLAUDE_CONFIG_DIR, so nothing touches ~/.claude. tools/e2e-spike/spike_env.clean_env gives the child a
minimal environment that ALWAYS carries the anti-update variables: without them a clean claude replaced
the pinned claude.exe once (spike_env.py's header).

Usage (Git Bash: prefix MSYS_NO_PATHCONV=1 so the folder argument is not rewritten):
    python -I interrupt_probe.py <port> <root> e1|e2|e3|e3b|e5
    python -I interrupt_probe.py <port> <root> e4 <fork session id printed by e3>
    <port>  the port the mock listens on (start it first: python -I mockllm.py <port> <root>\\requests.jsonl)
    <root>  a scratch folder: <root>\\cfg = the config dir, <root>\\work = the cwd, <root>\\requests.jsonl =
            the mock's log, which the probe reads to see whether a claude sent a request by itself.
Environment:
    E2_WAIT  seconds e2 waits after the turn went busy before the Esc (default 6). The mock holds every
             response ~2.5 s, so a short wait lands in the FIRST tool call and a long one (16) inside the
             second call's `sleep 25`; read the printed rows to see which shape the Esc made.
Experiments (what 2.1.291 did is in README.md):
    e1   Esc while the reply streams ([Request interrupted by user] under the partial reply); resume without
         the line (no request), append the rewind line, resume again (the prompt is sent again).
    e2   Esc during a running Bash call ([Request interrupted by user for tool use] over rejected results);
         resume without the line, append it, resume again (the turn goes on).
    e3   A source turn waits on its 2nd request (tail = a tool result); fork it while it runs: does the fork
         continue the turn by itself? The fork gets ONE Esc once it is busy; its transcript is shown.
    e3b  Like e3, but the source is INSIDE a running Bash call (tail = a tool_use without a result).
    e4   Append the rewind line to the interrupted fork of e3 and resume it: does it continue the turn?
    e5   Esc BEFORE any output (no row is written): does a plain resume send the prompt again by itself?
"""
import json
import os
import re
import sys
import time
import uuid

#: This kit's folder (tools/interrupt-probe) and the repository root two levels up, so the kit runs from
#: any checkout and finds the ConPTY driver and the spike's environment helpers in it.
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, ".claude", "skills", "claude-wrap-ConPTY", "scripts"))
sys.path.insert(0, os.path.join(REPO, "tools", "e2e-spike"))
from conpty_claude import ClaudeWrap  # noqa: E402
from spike_env import CLAUDE, clean_env, seed_config  # noqa: E402

#: Claude Code's interrupt row while a reply streams (its bundle's `VS`).
VS = "[Request interrupted by user]"
#: Claude Code's interrupt row while a tool call runs (its bundle's `gu`).
GU = "[Request interrupted by user for tool use]"
#: The meta prompt Claude Code sends when a resume continues an unfinished turn.
CONTINUE = "Continue from where you left off."


def encode_cwd(cwd):
    """Claude Code's project-folder name for a working folder.

    Args:
        cwd: The working folder.
    Returns:
        The folder name under <config>/projects: every character outside [A-Za-z0-9] becomes "-".
    """
    return re.sub(r"[^A-Za-z0-9]", "-", cwd)


class Probe:
    """One probe run: the mock's port, the scratch folders, and readers over the transcripts and the log."""

    def __init__(self, port, root):
        """Prepare the scratch folders and pre-answer Claude Code's first-run screens.

        Args:
            port: The mock's port on 127.0.0.1.
            root: The scratch folder (cfg, work and the mock's requests.jsonl live in it).
        Raises:
            OSError: When a folder or a seeded config file cannot be created or written.
            ValueError: When an existing seeded config file is not JSON (seed_config never overwrites it).
        """
        self.port = port
        self.root = root
        self.cfg = os.path.join(root, "cfg")
        self.cwd = os.path.join(root, "work")
        self.log = os.path.join(root, "requests.jsonl")
        os.makedirs(self.cwd, exist_ok=True)
        seed_config(self.cfg, self.cwd)

    def env(self):
        """The child's environment.

        Returns:
            spike_env.clean_env's minimal environment (the mock's URL, the scratch config folder, the
            anti-update variables) plus CLAUDE_CODE_RESUME_INTERRUPTED_TURN=1, the switch this machine's
            ~/.claude/settings.json sets: without it a resume never continues a turn by itself.
        """
        return clean_env(self.port, self.cfg, {"CLAUDE_CODE_RESUME_INTERRUPTED_TURN": "1"})

    def tpath(self, sid):
        """The transcript file of a session in the scratch config folder.

        Args:
            sid: The session id.
        Returns:
            <cfg>/projects/<encoded cwd>/<sid>.jsonl (the file may not exist yet).
        """
        return os.path.join(self.cfg, "projects", encode_cwd(self.cwd), sid + ".jsonl")

    def rows(self, sid):
        """Every line of a session's transcript, parsed.

        Args:
            sid: The session id.
        Returns:
            A list of dicts in file order; a line that is not JSON reads as {"type": "<torn>"}, and a missing
            or unreadable file reads as an empty list (a probe reports what it sees and goes on).
        """
        out = []
        try:
            with open(self.tpath(sid), encoding="utf-8") as f:
                for line in f:
                    try:
                        out.append(json.loads(line))
                    except ValueError:
                        out.append({"type": "<torn>"})
        except OSError:
            pass
        return out

    def start(self, args):
        """Start a claude on a new ConPTY in the scratch working folder.

        Args:
            args: The claude arguments after --dangerously-skip-permissions (e.g. ["--session-id", sid]).
        Returns:
            The started ClaudeWrap (150 x 42 cells); the caller ends it with exit_claude or close().
        Raises:
            FileNotFoundError: When the claude.exe of spike_env.CLAUDE does not exist.
            RuntimeError: When the ConPTY or the child cannot be started.
        """
        w = ClaudeWrap(cwd=self.cwd, exe=CLAUDE, args=["--dangerously-skip-permissions", *args], cols=150, rows=42,
                       env=self.env(), strip_nesting_env=True)
        w.start()
        return w

    def requests(self, sid, since):
        """The main-loop requests the mock logged for a session.

        Args:
            sid: The session id (the mock logs Claude Code's session header).
            since: A time.time() value; earlier requests are left out.
        Returns:
            The log records (dicts) of POST requests for `sid` at or after `since`, without the side requests
            (the session-title and compaction requests quote the prompt but continue nothing). A missing log
            reads as no requests.
        """
        out = []
        try:
            with open(self.log, encoding="utf-8") as f:
                for line in f:
                    try:
                        e = json.loads(line)
                    except ValueError:
                        continue
                    if e.get("method") != "POST" or e.get("t", 0) < since or e.get("session") != sid:
                        continue
                    if str(e.get("why", "")).startswith(("title", "compact")):
                        continue
                    out.append(e)
        except OSError:
            pass
        return out


def content_text(content):
    """The text of a message's content.

    Args:
        content: A message's "content": a string, or a list of blocks.
    Returns:
        The string itself, the joined text of the list's text blocks, or "" for anything else.
    """
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        return "".join(b.get("text", "") for b in content if isinstance(b, dict) and b.get("type") == "text")
    return ""


def is_tool_result(row):
    """Whether a user row carries a tool result.

    Args:
        row: A parsed transcript line.
    Returns:
        True when the row's message content is a list whose first block is a tool_result.
    """
    c = (row.get("message") or {}).get("content")
    return isinstance(c, list) and bool(c) and isinstance(c[0], dict) and c[0].get("type") == "tool_result"


def describe(row):
    """One line about a transcript row, for the printed shapes.

    Args:
        row: A parsed transcript line.
    Returns:
        "<uuid8><-<parent8> A[blocks] ..." for an assistant row, "U[...]" for a user row (tool result,
        META, SHUTDOWN and forkedFrom marked), the leaf fields of a last-prompt line, the subtype of a
        system / attachment / progress row, or the bare type of anything else.
    """
    t = row.get("type")
    u = (row.get("uuid") or "")[:8]
    p = (row.get("parentUuid") or "")[:8]
    msg = row.get("message") or {}
    c = msg.get("content")
    if t == "assistant":
        kinds = []
        if isinstance(c, list):
            for b in c:
                if isinstance(b, dict):
                    kinds.append("tool_use:" + str(b.get("name")) if b.get("type") == "tool_use" else str(b.get("type")))
        return "%s<-%s A[%s] id=%s stop=%s%s" % (u, p, ",".join(kinds), str(msg.get("id"))[-6:], msg.get("stop_reason"),
                                                 " APIERR" if row.get("isApiErrorMessage") else "")
    if t == "user":
        if is_tool_result(row):
            r = c[0].get("content")
            r = content_text(r) if isinstance(r, list) else (r or "")
            return "%s<-%s U[tool_result err=%s '%s']" % (u, p, c[0].get("is_error"), r[:60].replace("\n", " "))
        return "%s<-%s U[%s'%s']%s%s" % (u, p, "META " if row.get("isMeta") else "", content_text(c)[:70].replace("\n", " "),
                                          " SHUTDOWN" if row.get("interruptedByShutdown") else "",
                                          " forkedFrom" if row.get("forkedFrom") else "")
    if t == "last-prompt":
        return "last-prompt leaf=%s explicit=%s rewound=%s" % ((row.get("leafUuid") or "")[:8], row.get("explicit"), row.get("rewound"))
    if t in ("system", "attachment", "progress"):
        sub = row.get("subtype") or (row.get("attachment") or {}).get("type") or ""
        return "%s<-%s %s %s" % (u, p, t, sub)
    return t


def show_rows(probe, sid, label, last=14):
    """Print the last rows of a session's transcript.

    Args:
        probe: The Probe.
        sid: The session id.
        label: A heading for the printout.
        last: How many rows from the end to print.
    """
    rows = probe.rows(sid)
    print("--- %s: %s (%d lines)" % (label, sid[:8], len(rows)))
    for r in rows[-last:]:
        print("    " + describe(r))


def plan_target(rows):
    """Where the rewind line would point: a Python mirror of PlanInterruptRewind's walk (ApiErrorRewind.cpp).

    From the last conversation row (it must be an interrupt row) it walks up the parents: bookkeeping rows,
    the ONE interrupted response, Claude Code's own "Continue from where you left off." rows and, for the
    tool-use interrupt, the cut batch's results are stepped over; the first other user row is the target.
    The C++ planner refuses more shapes (denied results, meta and compaction targets, ...); this mirror only
    has to find the target for the probes' own simple turns. cpp_probe.py runs the real planner.

    Args:
        rows: The parsed transcript lines (Probe.rows).
    Returns:
        (target_uuid, kind, steps): kind is "prompt", "tool result" or "meta", and steps lists what was
        stepped over; or (None, why, steps) when the tail is no interrupt or the walk found no target.
    """
    by = {}
    for r in rows:
        if r.get("uuid") and not r.get("isSidechain"):
            by[r["uuid"]] = r
    marker = None
    for r in reversed(rows):
        if r.get("type") in ("user", "assistant") and not r.get("isSidechain"):
            if r.get("type") == "user" and content_text((r.get("message") or {}).get("content")) in (VS, GU):
                marker = r
            break
    if marker is None:
        return None, "the last conversation row is not an interrupt marker", []
    gu = content_text(marker["message"]["content"]) == GU
    steps, cur, saw_assistant, resp = [], marker, False, None
    # Bounded like the C++ walk, so a parent cycle in a damaged file ends the walk instead of the probe.
    for _ in range(100000):
        par = cur.get("parentUuid")
        if not par or par not in by:
            return None, "the chain left the file", steps
        cur = by[par]
        t = cur.get("type")
        if t in ("system", "attachment", "progress"):
            steps.append("skip " + t)
            continue
        if t == "assistant":
            mid = (cur.get("message") or {}).get("id")
            # Every row of ONE streamed response shares its message id; a second id above the interrupt means
            # a completed reply sits above it, which a rewind would hide (the planner's UnknownShape).
            if saw_assistant and resp and mid and mid != resp:
                return None, "two responses above the interrupt", steps
            resp = resp or mid
            saw_assistant = True
            steps.append("skip partial assistant")
            continue
        if t == "user":
            text = content_text((cur.get("message") or {}).get("content"))
            if cur.get("isMeta") and text == CONTINUE:
                steps.append("skip continue meta")
                continue
            if is_tool_result(cur):
                # Under a tool-use interrupt the results of the CUT batch sit between the interrupt and the
                # response that asked for them: they are the rejections, not the turn's last real step.
                if gu and not saw_assistant:
                    steps.append("skip interrupted batch's result")
                    continue
                return cur["uuid"], "tool result", steps
            if cur.get("isMeta"):
                return cur["uuid"], "meta", steps
            return cur["uuid"], "prompt", steps
    return None, "no end", steps


def append_marker(probe, sid, leaf):
    """Append the rewind line (the shape Agentmaster appends) to a session's transcript.

    Args:
        probe: The Probe.
        sid: The session id.
        leaf: The uuid the resume should continue from (plan_target's target).
    Raises:
        OSError: When the transcript cannot be appended to.
    """
    rows = probe.rows(sid)
    last_prompt = ""
    for r in rows:
        if r.get("type") == "last-prompt" and isinstance(r.get("lastPrompt"), str):
            last_prompt = r["lastPrompt"]
    # explicit:true holds the leaf until a later row; rewound:true would stop the resume from continuing.
    line = {"type": "last-prompt", "lastPrompt": last_prompt, "leafUuid": leaf, "explicit": True, "sessionId": sid}
    with open(probe.tpath(sid), "ab") as f:
        f.write((json.dumps(line, separators=(",", ":"), ensure_ascii=False) + "\n").encode("utf-8"))
    print("appended rewind line: leaf=%s" % leaf[:8])


def wait_busy(w, timeout):
    """Wait until the claude shows a turn running.

    Args:
        w: The ClaudeWrap.
        timeout: Seconds to wait at most.
    Returns:
        The seconds it took, or None when the turn never showed within `timeout`.
    """
    t0 = time.time()
    while time.time() - t0 < timeout:
        if w.is_busy():
            return time.time() - t0
        time.sleep(0.1)
    return None


def exit_claude(w):
    """End a claude gracefully (/exit, so its exit-time re-stamp runs), then close the ConPTY.

    A failed wait for idle is reported and the exit goes on: a probe keeps its printout going.

    Args:
        w: The ClaudeWrap.
    """
    try:
        w.wait_idle(timeout=30, quiet=1.0)
    except Exception as e:  # noqa: BLE001 - a probe: report and go on (ReadyTimeout and the like)
        print("  (not idle before /exit: %s)" % str(e).splitlines()[0])
    # An Esc before any output puts the prompt BACK into the box (2.1.291): End + Ctrl+U clears it, or
    # "/exit" is appended to it and submitted as a new prompt.
    w.send_raw("\x1b[F")
    time.sleep(0.2)
    for _ in range(3):
        w.send_raw("\x15")
        time.sleep(0.2)
    w.send_prompt("/exit")
    t0 = time.time()
    while w.alive and time.time() - t0 < 15:
        time.sleep(0.2)
    print("  claude exited: %s after %.1fs" % (not w.alive, time.time() - t0))
    w.close()


def resume_and_watch(probe, sid, extra_args, watch_s, label):
    """Resume a session and report whether it sent a main request by itself (= it continued).

    Args:
        probe: The Probe.
        sid: The session id.
        extra_args: More claude arguments after --resume <sid>.
        watch_s: Seconds to watch for a request.
        label: A heading for the printout.
    Returns:
        (w, reqs): the still running ClaudeWrap (the caller ends it) and the requests seen.
    Raises:
        FileNotFoundError, RuntimeError: When the claude cannot be started (the ConPTY is closed first when
            it was opened).
    """
    t0 = time.time()
    w = probe.start(["--resume", sid, *extra_args])
    try:
        try:
            w.wait_ready(timeout=40)
        except Exception as e:  # noqa: BLE001 - a busy resume never shows a ready box: report and watch on
            print("  (ready wait: %s)" % str(e).splitlines()[0])
        deadline = time.time() + watch_s
        busy_at = None
        while time.time() < deadline:
            if busy_at is None and w.is_busy():
                busy_at = time.time() - t0
            if probe.requests(sid, t0) and busy_at is not None:
                break
            time.sleep(0.25)
        reqs = probe.requests(sid, t0)
        print("=== %s: %d main request(s) within %ds of the resume; busy seen at %s" %
              (label, len(reqs), watch_s, "%.1fs" % busy_at if busy_at else "never"))
        for e in reqs[:3]:
            print("    request: why=%s answer=%s" % (e.get("why"), json.dumps(e.get("answer"))[:120]))
        return w, reqs
    except Exception:
        w.close()
        raise


def e1(probe):
    """Esc while a reply streams; resume without and with the rewind line.

    Args:
        probe: The Probe.
    Raises:
        ReadyTimeout, ModalError: When the first claude never shows its input box (conpty_claude).
    """
    sid = str(uuid.uuid4())
    w = probe.start(["--session-id", sid])
    w.wait_ready(timeout=40)
    w.send_prompt('e1 prompt [[mock: say "partial text"; delay 15000; reply "the end"]]')
    print("busy after %s s" % wait_busy(w, 15))
    time.sleep(3.0)  # "partial text" is streamed; the response then pauses
    w.interrupt()
    time.sleep(3.0)
    exit_claude(w)
    rows = probe.rows(sid)
    show_rows(probe, sid, "after Esc + /exit")
    target, kind, steps = plan_target(rows)
    print("plan: target=%s kind=%s steps=%s" % (target and target[:8], kind, steps))
    w, reqs = resume_and_watch(probe, sid, [], 12, "resume WITHOUT the rewind line")
    exit_claude(w)
    if not target:
        return
    append_marker(probe, sid, target)
    w, reqs = resume_and_watch(probe, sid, [], 25, "resume WITH the rewind line")
    try:
        w.wait_idle(timeout=40, quiet=1.0)
    except Exception as e:  # noqa: BLE001 - a probe: report and go on
        print("  (idle wait: %s)" % str(e).splitlines()[0])
    exit_claude(w)
    show_rows(probe, sid, "after the uninterrupted resume", last=10)


def e2(probe):
    """Esc during a tool call; resume without and with the rewind line (E2_WAIT picks which call).

    Args:
        probe: The Probe.
    Raises:
        ReadyTimeout, ModalError: When the first claude never shows its input box (conpty_claude).
        ValueError: When E2_WAIT is not a number.
    """
    sid = str(uuid.uuid4())
    w = probe.start(["--session-id", sid])
    w.wait_ready(timeout=40)
    w.send_prompt('e2 prompt [[mock: tool Bash {"command": "echo one", "description": "first"}; '
                  'tool Bash {"command": "sleep 25", "description": "long"}; reply "done"]]')
    print("busy after %s s" % wait_busy(w, 15))
    time.sleep(float(os.environ.get("E2_WAIT", "6")))
    w.interrupt()
    time.sleep(3.0)
    exit_claude(w)
    rows = probe.rows(sid)
    show_rows(probe, sid, "after Esc in the tool + /exit", last=16)
    target, kind, steps = plan_target(rows)
    print("plan: target=%s kind=%s steps=%s" % (target and target[:8], kind, steps))
    w, reqs = resume_and_watch(probe, sid, [], 12, "resume WITHOUT the rewind line")
    exit_claude(w)
    if not target:
        return
    append_marker(probe, sid, target)
    w, reqs = resume_and_watch(probe, sid, [], 25, "resume WITH the rewind line")
    time.sleep(4.0)
    w.interrupt()  # the re-run sleep 25 would take long - stop it; the transcript shows what continued
    time.sleep(2.0)
    exit_claude(w)
    show_rows(probe, sid, "after the uninterrupted resume", last=14)


def fork_while_running(probe, script, settle_s, label):
    """Fork a session while its turn runs, time the fork's continuation, and stop it with one Esc.

    Prints when the fork's input box showed, when it went busy and when it sent its first main request
    (seconds after its launch), the scratch config's heartbeat files, and the fork's last rows.

    Args:
        probe: The Probe.
        script: The source's prompt with its [[mock ...]] script.
        settle_s: Seconds the source runs before the fork starts.
        label: Where the source stands when it is forked, for the printout.
    Returns:
        The fork's session id.
    Raises:
        ReadyTimeout, ModalError: When the source never shows its input box (conpty_claude).
    """
    src = str(uuid.uuid4())
    w = probe.start(["--session-id", src])
    w.wait_ready(timeout=40)
    w.send_prompt(script)
    print("source busy after %s s" % wait_busy(w, 15))
    time.sleep(settle_s)
    show_rows(probe, src, "source while running (%s)" % label, last=8)
    fork = str(uuid.uuid4())
    t0 = time.time()
    f = probe.start(["--resume", src, "--fork-session", "--session-id", fork])
    ready = None
    busy = None
    first_req = None
    while time.time() - t0 < 30:
        if ready is None and f.input_box_ready():
            ready = time.time() - t0
        if busy is None and f.is_busy():
            busy = time.time() - t0
        if first_req is None and probe.requests(fork, t0):
            first_req = time.time() - t0
        if busy is not None and first_req is not None:
            break
        time.sleep(0.1)
    print("=== fork %s: box ready %s, busy %s, first main request %s (s after launch)" %
          (fork[:8], ready and "%.1f" % ready, busy and "%.1f" % busy, first_req and "%.1f" % first_req))
    # The heartbeat files are what the fork stop watch reads: each names its session and its status.
    presence = os.path.join(probe.cfg, "sessions")
    try:
        for name in os.listdir(presence):
            if name.endswith(".json"):
                with open(os.path.join(presence, name), encoding="utf-8") as fh:
                    d = json.load(fh)
                print("    presence %s sid=%s status=%s" % (name, str(d.get("sessionId"))[:8], d.get("status")))
    except (OSError, ValueError) as e:
        print("    (heartbeat files not read: %s)" % e)
    if busy is not None:
        f.interrupt()
        print("  Esc sent to the fork %.1fs after launch" % (time.time() - t0))
        time.sleep(3.0)
    exit_claude(f)
    show_rows(probe, fork, "fork after Esc + /exit", last=10)
    exit_claude(w)
    return fork


def e3(probe):
    """Fork a source whose turn waits on its 2nd request; prints the fork id for e4.

    Args:
        probe: The Probe.
    Raises:
        ReadyTimeout, ModalError: As fork_while_running.
    """
    fork = fork_while_running(probe, 'e3 prompt [[mock: tool Bash {"command": "echo one", "description": "first"}; '
                                     'delay 30000; reply "done"]]', 5.0, "2nd request in flight")
    print("fork id for e4: " + fork)


def e3b(probe):
    """Fork a source that is inside a running Bash call (its tail is a tool_use with no result).

    Args:
        probe: The Probe.
    Raises:
        ReadyTimeout, ModalError: As fork_while_running.
    """
    fork_while_running(probe, 'e3b prompt [[mock: tool Bash {"command": "sleep 40", "description": "long"}; '
                              'reply "done"]]', 9.0, "in a running Bash call")


def e4(probe, fork):
    """Rewind the interrupted fork of e3 and resume it: does it continue the turn?

    Args:
        probe: The Probe (the same root as the e3 run).
        fork: The fork's session id e3 printed.
    Raises:
        OSError: When the rewind line cannot be appended.
    """
    rows = probe.rows(fork)
    show_rows(probe, fork, "interrupted fork", last=10)
    target, kind, steps = plan_target(rows)
    print("plan: target=%s kind=%s steps=%s" % (target and target[:8], kind, steps))
    if not target:
        return
    append_marker(probe, fork, target)
    w, reqs = resume_and_watch(probe, fork, [], 25, "resume the fork WITH the rewind line")
    time.sleep(3.0)
    w.interrupt()
    time.sleep(2.0)
    exit_claude(w)
    show_rows(probe, fork, "fork after the uninterrupted resume", last=10)


def e5(probe):
    """Esc BEFORE any output: 2.1.291 writes no row and puts the prompt back in the box. Does a plain
    resume then send the prompt again by itself?

    Args:
        probe: The Probe.
    Raises:
        ReadyTimeout, ModalError: When the first claude never shows its input box (conpty_claude).
    """
    sid = str(uuid.uuid4())
    w = probe.start(["--session-id", sid])
    w.wait_ready(timeout=40)
    w.send_prompt('e5 prompt [[mock: delay 15000; reply "late"]]')
    print("busy after %s s" % wait_busy(w, 15))
    time.sleep(2.0)
    w.interrupt()
    time.sleep(3.0)
    exit_claude(w)
    show_rows(probe, sid, "after Esc before output + /exit", last=6)
    w, reqs = resume_and_watch(probe, sid, [], 15, "resume (no edit)")
    time.sleep(1.0)
    w.interrupt()
    time.sleep(2.0)
    exit_claude(w)


def main(argv):
    """Run one experiment.

    Args:
        argv: <port> <root> <experiment> [fork id for e4].
    Returns:
        0.
    Raises:
        SystemExit: With the usage text for an unknown experiment or missing arguments.
    """
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    if len(argv) < 3:
        raise SystemExit(__doc__)
    port, root, exp = int(argv[0]), argv[1], argv[2]
    probe = Probe(port, root)
    if exp == "e1":
        e1(probe)
    elif exp == "e2":
        e2(probe)
    elif exp == "e3":
        e3(probe)
    elif exp == "e3b":
        e3b(probe)
    elif exp == "e5":
        e5(probe)
    elif exp == "e4" and len(argv) > 3:
        e4(probe, argv[3])
    else:
        raise SystemExit(__doc__)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
