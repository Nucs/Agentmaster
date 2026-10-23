"""Run the scripted mock model: ``python -m mockllm --port 8787 --out C:\\e2e\\out\\mock``.

Claude Code then reaches it with ``ANTHROPIC_BASE_URL=http://127.0.0.1:8787`` and any
``ANTHROPIC_AUTH_TOKEN`` (never ANTHROPIC_API_KEY: that raises Claude Code's "use this API key?" question).
"""
from __future__ import annotations

import argparse
import os
import sys

from .server import make_server
from .state import MockState


def main(argv: list[str]) -> int:
    """Parse the command line and serve until killed.

    Args:
        argv: The arguments after the module name.
    Returns:
        Never returns normally (serve_forever); 0 after a Ctrl+C.
    Raises:
        OSError: When the port is taken or the output folder cannot be created.
    """
    parser = argparse.ArgumentParser(prog="mockllm", description="The e2e suite's scripted Anthropic Messages API.")
    parser.add_argument("--port", type=int, default=8787, help="TCP port on 127.0.0.1 (default 8787)")
    parser.add_argument("--out", default="", help="folder for requests.jsonl (and bodies.jsonl with --full)")
    parser.add_argument("--full", action="store_true", help="also log every request body (system prompt + conversation)")
    parser.add_argument("--scenarios", default="", help="folder of <name>.mock files for `use @name`")
    parser.add_argument("--floor-ms", type=int, default=2500, help="the turn floor (default 2500; 0 = instant turns)")
    parser.add_argument("--default-reply", default="OK", help="the answer of a turn whose prompt holds no script")
    args = parser.parse_args(argv)
    log_path = full_path = None
    if args.out:
        os.makedirs(args.out, exist_ok=True)
        log_path = os.path.join(args.out, "requests.jsonl")
        if args.full:
            full_path = os.path.join(args.out, "bodies.jsonl")
    state = MockState(log_path, full_path, floor_ms=args.floor_ms, default_reply=args.default_reply)
    server = make_server(args.port, state, args.scenarios or None)
    print(f"mockllm listening on 127.0.0.1:{server.server_address[1]}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        return 0
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
