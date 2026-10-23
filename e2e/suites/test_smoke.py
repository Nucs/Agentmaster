"""The suite's own foundation: fixture F-S works end to end, and every surface the other suites read shows it.

If this fails, the problem is the environment or the driver (the mock, claude.exe, the launch path, a page
object's locator), not a feature: fix it before reading any other failure.
"""
import pytest


@pytest.mark.p0
@pytest.mark.flow("F-S")
def test_launched_session_shows_on_every_surface(am, s1, mock):
    """F-S: a session launched through the Agent Manager's launch row, its first turn done.

    Args:
        am: The app.
        s1: The session in t1.
        mock: The scripted model.
    """
    w = am.window
    # [ENG] the record: titled by its folder, Waiting for you, a managed (not adopted) Claude session.
    rec = s1.record()
    assert rec.title == "t1" and rec.state == "WaitingForInput" and not rec.external and rec.kind == "Claude"
    # [MOCK] the first prompt reached the model in THIS session (Claude Code's session id == Agentmaster's).
    assert any(p["session"] == s1.id for p in mock.prompts()), mock.prompts()
    # [TR] Claude Code wrote the transcript under the isolated config folder.
    assert "ready" in " ".join(am.claude.user_prompts(s1.id))
    # [UI] the tab, selected; the terminal shows Claude Code's empty input box.
    assert w.tabs.selected().title == "t1"
    assert w.terminal.input_box().draft == ""
    # [UI] the side panel: a card in the session's state, the count line.
    w.side_panel.wait_card("t1", state="waiting-for-you")
    assert w.side_panel.count_line() == "1 session in this window"
    # [UI] the Agent Manager page: a card in Waiting-for-you, a tree row.
    page = w.manager.open()
    page.wait_card("t1", column="Waiting-for-you")
    assert page.tree_row("t1").state == "waiting-for-you"
    # [LOG] Agentmaster seeded the workspace trust itself (the suite never seeds it).
    assert am.profile.hooks.find(r"\[trust\]")
