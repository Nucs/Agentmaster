"""amdriver: drives Agentmaster like a user in the end-to-end suite's Windows test desktop.

Layers (doc/agentmaster/E2E_TESTING.md §7):
    win32, inputs     native windows, foreground, clipboard; real mouse and keyboard (SendInput)
    uia               UI Automation: find controls, read their state, invoke them
    waits             condition waits (never sleeps)
    app               the Agentmaster process: start with an isolated profile, windows, close, kill
    profile           the profile's files: sessions.json, window records, settings, the logs with read marks
    claude            Claude Code's isolated config folder (seeds) and its transcripts
    terminal          a tab's terminal: read the screen, find Claude's input box, send a prompt (verified)
    ui                page objects: the tab strip, the context menus, the Agent Manager page, the side panel
    evidence          screenshots, UIA dumps and log excerpts for a failing test

⚠ COM must be initialized multi-threaded for UI Automation clients (Microsoft's recommendation); comtypes reads
sys.coinit_flags when it is FIRST imported, so it is set here, before any submodule imports comtypes.
"""
import sys

sys.coinit_flags = 0  # COINIT_MULTITHREADED
