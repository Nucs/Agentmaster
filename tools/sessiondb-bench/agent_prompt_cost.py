# SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
"""What the session agent's system prompt and tool list cost in Claude Code's context (SESSIONS_AGENT.md 8.7).

It starts the real claude.exe the way the harness does (stream-json both ways, in-process "sdk" MCP
servers, --tools "", no session persistence, no setting sources), answers `initialize` and each
server's `tools/list`, waits until every server has listed its tools, then sends the control request
`get_context_usage` and prints the token counts Claude Code reports: the system prompt, all MCP
tools, and each tool. NO user message is sent, so no model request is made.

Two configurations, both read from the repo:
  shipped  - the system prompt and the two tools of SessionSearchTools.cpp (kSystemPrompt,
             kSearchSchema, kShowSchema and the two descriptions), as the source holds them now
  proposed - SESSIONS_AGENT.md section 8.6 (the prompt; the doc's line wrap undone) and
             SESSIONS_AGENT.tools.json (the tools)

Run it again after any change of a description or a schema: a description is part of the model's
prompt (SESSIONS_AGENT.md 8.1). It strips the variables of a parent Claude Code session from the
child's environment and marks the child AM_HARNESS, so the Fleet Observer skips it.

Usage: python agent_prompt_cost.py [<work folder>]    (default: %TEMP%\\agentmaster-agent-cost)
"""

import json
import os
import queue
import re
import subprocess
import sys
import tempfile
import threading
import time

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
CLAUDE = os.path.expanduser(os.path.join("~", ".local", "bin", "claude.exe"))
# Variables of a parent Claude Code session (this script usually runs inside one): a child that
# inherits them reports the parent's tab, can reach its messaging socket, or takes its effort level.
STRIP = [
    "CLAUDECODE", "CLAUDE_CODE_CHILD_SESSION", "CLAUDE_CODE_SESSION_ID", "CLAUDE_CODE_ENTRYPOINT",
    "AI_AGENT", "CLAUDE_CODE_MESSAGING_SOCKET", "CLAUDE_CODE_MESSAGING_TOKEN",
    "CLAUDE_CODE_SESSION_ATTENDED", "CLAUDE_CODE_EXECPATH", "CLAUDE_PID", "AM_SESSION",
    "WT_SESSION", "CCMGR_HOOK_PIPE", "CCMGR_SESSION_ID", "CLAUDE_CODE_EFFORT_LEVEL",
    "CLAUDE_EFFORT", "DEBUG", "CLAUDE_CODE_PLUGIN_DIRS",
]


def wide_concat(block: str) -> str:
    """Join the L"..." pieces of a C++ wide-string concatenation.

    Args:
        block: the C++ source between `=` and `;` of the array's definition.
    Returns:
        The string, with the C escapes (\\n, \\", \\\\) decoded.
    """
    parts = re.findall(r'L"((?:[^"\\]|\\.)*)"', block)
    return "".join(p.encode("utf-8").decode("unicode_escape") for p in parts)


def raw_concat(block: str) -> str:
    """Join the LR"(...)" raw-string pieces of a C++ concatenation.

    Args:
        block: the C++ source of the concatenation.
    Returns:
        The joined text, verbatim.
    """
    return "".join(re.findall(r'LR"\((.*?)\)"', block, re.S))


def unwrap_prompt(block: str) -> str:
    """Undo the doc's line wrap of the system prompt.

    Args:
        block: the fenced block of SESSIONS_AGENT.md 8.6, wrapped at about 100 columns.
    Returns:
        The prompt with each paragraph and each numbered rule on one line, as the C++ will hold it.
    """
    out, cur = [], []
    for line in block.strip("\n").split("\n"):
        if not line.strip():
            if cur:
                out.append(" ".join(cur))
                cur = []
            out.append("")
        elif re.match(r"^\d+\. ", line) and cur:
            out.append(" ".join(cur))
            cur = [line.strip()]
        else:
            cur.append(line.strip())
    if cur:
        out.append(" ".join(cur))
    return "\n".join(out).strip() + "\n"


def shipped_config() -> tuple:
    """Read the shipped prompt and tools from SessionSearchTools.cpp.

    Returns:
        (prompt text, {"<server>": [tool, ...]}).
    Raises:
        OSError: the source file cannot be read.
        AttributeError: a definition was not found (the source changed shape: update the patterns).
        ValueError: a schema is not valid JSON.
    """
    src = open(os.path.join(REPO, "src", "cascadia", "TerminalApp", "AgentMaster", "SessionSearchTools.cpp"), encoding="utf-8").read()
    prompt = wide_concat(re.search(r"const wchar_t kSystemPrompt\[\] =(.*?);\n", src, re.S).group(1))
    search_schema = json.loads(raw_concat(re.search(r"const wchar_t kSearchSchema\[\] =(.*?);\n", src, re.S).group(1)))
    show_schema = json.loads(raw_concat(re.search(r"const wchar_t kShowSchema\[\] =(.*?);\n", src, re.S).group(1)))
    search_desc = re.search(r'search\.description = L"((?:[^"\\]|\\.)*)"', src).group(1)
    show_desc = re.search(r'show\.description = L"((?:[^"\\]|\\.)*)"', src).group(1)
    servers = {
        "sessions": [{"name": "search", "description": search_desc, "inputSchema": search_schema, "annotations": {"readOnlyHint": True}}],
        "display": [{"name": "show", "description": show_desc, "inputSchema": show_schema}],
    }
    return prompt, servers


def proposed_config() -> tuple:
    """Read the proposed prompt (SESSIONS_AGENT.md 8.6) and tools (SESSIONS_AGENT.tools.json).

    Returns:
        (prompt text, {"<server>": [tool, ...]}).
    Raises:
        OSError: a file cannot be read.
        IndexError: the doc has no fenced block under "8.6 The system prompt".
        ValueError: the tools file is not valid JSON.
    """
    doc = open(os.path.join(REPO, "doc", "agentmaster", "SESSIONS_AGENT.md"), encoding="utf-8").read()
    block = doc.split("### 8.6 The system prompt", 1)[1].split("```", 2)[1]
    block = block[block.index("\n") + 1:]  # the fence's own line
    tools = json.load(open(os.path.join(REPO, "doc", "agentmaster", "SESSIONS_AGENT.tools.json"), encoding="utf-8"))
    return unwrap_prompt(block), tools["servers"]


def measure(prompt: str, servers: dict, work: str) -> dict:
    """Start claude.exe with this prompt and these tool servers and read its context report.

    Args:
        prompt: the system prompt (it replaces Claude Code's).
        servers: {"<server>": [{"name", "description", "inputSchema", "annotations"?}, ...]}.
        work: a scratch folder (the child's working folder and the prompt file go there).
    Returns:
        The `get_context_usage` answer (a dict), without its drawing grid.
    Raises:
        RuntimeError: initialize failed, or no context report came within the time limits.
        OSError: claude.exe cannot be started.
    """
    os.makedirs(work, exist_ok=True)
    prompt_path = os.path.join(work, "system-prompt.txt")
    with open(prompt_path, "w", encoding="utf-8") as fh:
        fh.write(prompt)
    mcp = {"mcpServers": {name: {"type": "sdk", "name": name} for name in servers}}
    cmd = [CLAUDE, "--output-format", "stream-json", "--input-format", "stream-json", "--verbose",
           "--system-prompt-file", prompt_path, "--allowedTools", ",".join(f"mcp__{n}" for n in servers),
           "--tools", "", "--model", "claude-sonnet-5-5", "--permission-mode", "dontAsk",
           "--strict-mcp-config", "--settings", json.dumps({"verbose": False}), "--mcp-config", json.dumps(mcp),
           "--setting-sources", "", "--disable-slash-commands", "--no-session-persistence",
           "--thinking", "disabled", "--effort", "low"]
    env = {k: v for k, v in os.environ.items() if k not in STRIP}
    env["CLAUDE_CODE_ENTRYPOINT"] = "sdk-py"
    env["AM_HARNESS"] = "prompt-cost"  # the Fleet Observer skips a process with this mark
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            cwd=work, env=env, creationflags=0x08000000)  # CREATE_NO_WINDOW
    lines: "queue.Queue" = queue.Queue()
    errors: list = []

    def pump() -> None:
        """Move the child's stdout lines into the queue; None marks the end."""
        for raw in proc.stdout:
            lines.put(raw.decode("utf-8", "replace").rstrip("\r\n"))
        lines.put(None)

    threading.Thread(target=pump, daemon=True).start()
    threading.Thread(target=lambda: errors.append(proc.stderr.read().decode("utf-8", "replace")), daemon=True).start()
    listed: set = set()
    counter = [0]

    def send(obj: dict) -> None:
        """Write one stream-json line to the child."""
        proc.stdin.write((json.dumps(obj) + "\n").encode("utf-8"))
        proc.stdin.flush()

    def control(subtype: str) -> str:
        """Send a control request; returns its request id."""
        counter[0] += 1
        rid = f"req_{counter[0]}_cost"
        send({"type": "control_request", "request_id": rid, "request": {"subtype": subtype}})
        return rid

    def answer_mcp(server: str, msg: dict) -> dict:
        """Answer one JSON-RPC message of an in-process MCP server."""
        method = msg.get("method", "")
        mid = msg.get("id")
        if method == "tools/list":
            listed.add(server)
            return {"jsonrpc": "2.0", "id": mid, "result": {"tools": servers.get(server, [])}}
        if method == "initialize":
            return {"jsonrpc": "2.0", "id": mid, "result": {"protocolVersion": "2024-11-05", "serverInfo": {"name": server, "version": "1.0.0"}, "capabilities": {"tools": {}}}}
        if method.startswith("notifications/"):
            return {"jsonrpc": "2.0", "result": {}}
        return {"jsonrpc": "2.0", "id": mid, "error": {"code": -32601, "message": f"Method not found: {method}"}}

    def until(request_id: str, timeout: float):
        """Serve MCP traffic until the control answer `request_id` arrives (or the time is up)."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                line = lines.get(timeout=0.25)
            except queue.Empty:
                if proc.poll() is not None:
                    return None
                continue
            if line is None:
                return None
            try:
                obj = json.loads(line)
            except ValueError:
                continue  # a non-JSON line (none are expected in stream-json mode)
            req = obj.get("request", {})
            if obj.get("type") == "control_request" and req.get("subtype") == "mcp_message":
                resp = answer_mcp(req.get("server_name", ""), req.get("message", {}))
                send({"type": "control_response", "response": {"subtype": "success", "request_id": obj.get("request_id"), "response": {"mcp_response": resp}}})
            elif obj.get("type") == "control_response" and obj.get("response", {}).get("request_id") == request_id:
                return obj["response"]
        return None

    try:
        init = until(control("initialize"), 60)
        if not init or init.get("subtype") != "success":
            raise RuntimeError("initialize failed: " + "".join(errors)[:500])
        # The MCP servers connect after initialize; the report counts only listed tools.
        deadline = time.time() + 10
        while len(listed) < len(servers) and time.time() < deadline:
            until("never", 0.5)  # an id no answer carries: serve MCP traffic only
        usage = until(control("get_context_usage"), 60)
        if not usage or not isinstance(usage.get("response"), dict):
            raise RuntimeError("no context report: " + "".join(errors)[:500])
        body = usage["response"]
        body.pop("gridRows", None)
        return body
    finally:
        try:
            proc.stdin.close()
            proc.wait(timeout=15)
        except (OSError, subprocess.TimeoutExpired):
            proc.kill()  # it did not exit by itself after stdin closed


def main() -> int:
    """Measure both configurations and print the table rows of SESSIONS_AGENT.md 8.7.

    Returns:
        0, or 1 when a measurement failed (the reason is printed).
    """
    sys.stdout.reconfigure(encoding="utf-8")
    work = sys.argv[1] if len(sys.argv) > 1 else os.path.join(tempfile.gettempdir(), "agentmaster-agent-cost")
    for label, (prompt, servers) in (("shipped", shipped_config()), ("proposed", proposed_config())):
        try:
            body = measure(prompt, servers, os.path.join(work, label))
        except (RuntimeError, OSError) as exc:
            print(f"{label}: FAILED: {exc}")
            return 1
        cats = {c["name"]: c["tokens"] for c in body.get("categories", [])}
        tools = sorted(body.get("mcpTools", []), key=lambda t: -t["tokens"])
        print(f"{label}: system prompt {cats.get('System prompt', 0):,} tokens ({len(prompt):,} chars), "
              f"MCP tools {cats.get('MCP tools', 0):,} tokens, together {cats.get('System prompt', 0) + cats.get('MCP tools', 0):,}")
        print("   " + ", ".join(f"{t['name'].split('__')[-1]} {t['tokens']:,}" for t in tools))
    return 0


if __name__ == "__main__":
    sys.exit(main())
