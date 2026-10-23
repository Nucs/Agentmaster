"""mockllm: the end-to-end suite's scripted Anthropic Messages API (doc/agentmaster/E2E_TESTING.md §4).

A test writes the model's answers into the prompt it sends (``[[mock bash "git status"; reply "done"]]``), so a
real Claude Code finishes every turn in seconds, with real tool calls, dialogs, compactions, API errors and cut
streams — and the request log tells the test exactly what reached the model.

Modules: script (the language), classify (reading requests), state (log, gates, attempts, floors),
server (HTTP + the control API), client (the test side of the control API).
"""
