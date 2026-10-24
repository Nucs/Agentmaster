"""Unit tests of the script language (mockllm.script): parsing, planning, shorthands, tool-input checks.

Every rule a test script relies on is pinned here, so a change of the language cannot silently change what a
scripted turn does in the end-to-end suite.
"""
from __future__ import annotations

import pytest

from mockllm.script import (FILLER_LINE_CHARS, MAX_FILLER_CHARS, Element, Quoted, ScriptError, check_tool_input,
                            expand_shorthand, filler, find_scripts, parse_steps, plan)


def kinds(response) -> list:
    """The element kinds of a response (a compact assertion target).

    Args:
        response: A planned Response.
    Returns:
        [kind, ...] in plan order.
    """
    return [el.kind for el in response.elements]


class TestFindScripts:
    """Locating `[[mock ...]]` bodies in a message's text (nested scripts must not end the outer one)."""

    def test_simple(self):
        """One script is found with its exact body.

        Returns:
            None.
        Raises:
            AssertionError: When the body differs.
        """
        assert find_scripts('fix it [[mock reply "done"]]') == [' reply "done"']

    def test_nested_script_inside_a_string_does_not_end_the_outer_one(self):
        """A subagent's script inside a JSON string keeps the outer script whole (a regex would cut it).

        Returns:
            None.
        Raises:
            AssertionError: When the outer body is cut at the inner "]]".
        """
        text = 'go [[mock agent "[[mock reply \\"sub\\"]]"; reply "parent"]]'
        bodies = find_scripts(text)
        assert len(bodies) == 1
        assert bodies[0].strip().endswith('reply "parent"')

    def test_several_and_unterminated(self):
        """Several scripts are all found; an unterminated one is ordinary text.

        Returns:
            None.
        Raises:
            AssertionError: On a wrong count.
        """
        assert len(find_scripts('[[mock reply "a"]] and [[mock reply "b"]]')) == 2
        assert find_scripts('[[mock reply "a"') == []
        assert find_scripts("no script here") == []


class TestParseSteps:
    """Tokenizing a script body into steps."""

    def test_json_and_bare_arguments(self):
        """JSON values and bare words (statuses, key=value, durations) parse side by side.

        Returns:
            None.
        Raises:
            AssertionError: On a wrong parse.
        """
        steps = parse_steps('tool Bash {"command": "ls"}; error 529 overloaded_error "x" times=2; delay 3s')
        assert steps == [("tool", ["Bash", {"command": "ls"}]),
                         ("error", ["529", "overloaded_error", "x", "times=2"]),
                         ("delay", ["3s"])]

    def test_spike_colon_form(self):
        """The spike's leading colon (``[[mock: ...]]``) still parses.

        Returns:
            None.
        Raises:
            AssertionError: On a wrong parse.
        """
        assert parse_steps(': reply "x"') == [("reply", ["x"])]

    def test_unknown_step_fails_loudly(self):
        """A misspelled step is an error, never a guess (a typo must fail the test, not run something else).

        Returns:
            None.
        Raises:
            AssertionError: When no ScriptError is raised.
        """
        with pytest.raises(ScriptError, match="unknown step 'replyy'"):
            parse_steps('replyy "x"')

    def test_bad_json_fails_loudly(self):
        """A malformed JSON argument is an error with its position.

        Returns:
            None.
        Raises:
            AssertionError: When no ScriptError is raised.
        """
        with pytest.raises(ScriptError, match="does not parse"):
            parse_steps('tool Bash {"command": }')

    def test_a_quoted_text_holding_equals_is_never_an_option(self):
        """A JSON string argument with "=" in it stays the step's text (it was taken for a key=value option before
        the parser marked quoted texts); a bare word with "=" is still an option.

        Returns:
            None.
        Raises:
            AssertionError: When the quoted text is read as an option.
        """
        steps = parse_steps('bash "FOO=1 make"; say "x = 1"; error 429 rate_limit_error "a=b" times=1')
        assert isinstance(steps[0][1][0], Quoted) and steps[0][1][0] == "FOO=1 make"
        p = plan('bash "FOO=1 make"; reply "x = 1"')
        assert p.responses[0].elements[0].input["command"] == "FOO=1 make"
        assert p.responses[1].elements[0].text == "x = 1"
        q = plan('error 429 rate_limit_error "limit=1" times=1; reply "ok"')
        assert q.responses[0].fail == (429, "rate_limit_error", "limit=1", 1)


class TestPlan:
    """Grouping steps into the responses of one turn."""

    def test_bare_reply(self):
        """A body that does not start with a step name is the reply itself.

        Returns:
            None.
        Raises:
            AssertionError: On a wrong plan.
        """
        p = plan("some words here")
        assert len(p.responses) == 1
        assert p.responses[0].elements[0].text == "some words here"
        assert p.responses[0].final

    def test_tool_ends_the_response_and_reply_ends_the_script(self):
        """A tool call closes its response (tool_use); reply closes the script; later steps never run.

        Returns:
            None.
        Raises:
            AssertionError: On a wrong grouping.
        """
        p = plan('think "plan"; bash "git status"; say "x"; reply "done"; say "never"')
        assert [kinds(r) for r in p.responses] == [["thinking", "tool_use"], ["text", "text"]]
        assert p.responses[0].stop == "tool_use" and not p.responses[0].final
        assert p.responses[1].stop == "end_turn" and p.responses[1].final
        assert p.responses[0].elements[1].input == {"command": "git status", "description": "e2e mock command"}

    def test_trailing_say_forms_a_final_response(self):
        """Steps after the last tool call without a reply still end the turn.

        Returns:
            None.
        Raises:
            AssertionError: On a wrong plan.
        """
        p = plan('tool Read {"file_path": "a"}; say "tail"')
        assert len(p.responses) == 2 and p.responses[1].final

    def test_parallel_tools(self):
        """`tools` puts several tool calls in one response (both list and object forms).

        Returns:
            None.
        Raises:
            AssertionError: On a wrong plan.
        """
        p = plan('tools [["Read", {"file_path": "a"}], {"name": "Read", "input": {"file_path": "b"}}]; reply "ok"')
        assert kinds(p.responses[0]) == ["tool_use", "tool_use"]
        assert [el.input["file_path"] for el in p.responses[0].elements] == ["a", "b"]

    def test_expect_belongs_to_the_next_response(self):
        """`expect` checks the tool result that arrives with the NEXT request, so it attaches there.

        Returns:
            None.
        Raises:
            AssertionError: When it attaches to the wrong response.
        """
        p = plan('bash "echo hi"; expect "hi"; reply "done"')
        assert p.responses[0].expect == ""
        assert p.responses[1].expect == "hi"

    def test_bad_expect_regex(self):
        """An invalid regex in `expect` fails at planning time.

        Returns:
            None.
        Raises:
            AssertionError: When no ScriptError is raised.
        """
        with pytest.raises(ScriptError, match="not a regex"):
            plan('bash "x"; expect "("; reply "y"')

    def test_permanent_error_is_a_response_of_its_own(self):
        """`error` without times fails every attempt of its response (Claude Code then ends the turn).

        Returns:
            None.
        Raises:
            AssertionError: On a wrong plan.
        """
        p = plan('error 400 invalid_request_error "prompt is too long"')
        assert len(p.responses) == 1
        assert p.responses[0].fail == (400, "invalid_request_error", "prompt is too long", None)

    def test_error_times_attaches_to_the_next_response(self):
        """`error ... times=N` fails the first N attempts of the next response, then serves it.

        Returns:
            None.
        Raises:
            AssertionError: On a wrong plan.
        """
        p = plan('error 529 overloaded_error "busy" times=2; reply "after retries"')
        assert len(p.responses) == 1
        assert p.responses[0].fail == (529, "overloaded_error", "busy", 2)
        assert p.responses[0].elements[0].text == "after retries"

    def test_spike_error_form_with_a_fourth_number(self):
        """The spike's positional fourth argument means times=N.

        Returns:
            None.
        Raises:
            AssertionError: On a wrong plan.
        """
        p = plan('error 529 overloaded_error "busy" 1; reply "ok"')
        assert p.responses[0].fail[3] == 1

    def test_error_retry_options_become_headers(self):
        """`retry=no|yes` and `retry_after=<s>` give the failing attempts the API's x-should-retry / retry-after headers,
        on a permanent error and on a `times=N` one (carried to the response it fails); no option, no header.

        Returns:
            None.
        Raises:
            AssertionError: On a wrong header set.
        """
        final = plan('error 429 rate_limit_error "slow down" retry=no').responses[0]
        assert final.fail == (429, "rate_limit_error", "slow down", None)
        assert final.fail_headers == {"x-should-retry": "false"}
        later = plan('error 429 rate_limit_error "wait" times=1 retry_after=5; reply "after"').responses[0]
        assert later.fail == (429, "rate_limit_error", "wait", 1) and later.fail_headers == {"retry-after": "5"}
        both = plan('error 500 api_error "x" retry=yes retry_after=1.5').responses[0]
        assert both.fail_headers == {"x-should-retry": "true", "retry-after": "1.5"}
        assert plan('error 529 overloaded_error "x"').responses[0].fail_headers == {}

    def test_error_options_are_checked(self):
        """A misspelled or malformed `error` option fails the script instead of sending no header.

        Returns:
            None.
        Raises:
            AssertionError: When no ScriptError is raised.
        """
        with pytest.raises(ScriptError, match="unknown option"):
            plan('error 429 rate_limit_error "x" retry_afer=5')
        with pytest.raises(ScriptError, match="retry must be yes or no"):
            plan('error 429 rate_limit_error "x" retry=maybe')
        with pytest.raises(ScriptError, match="number of seconds"):
            plan('error 429 rate_limit_error "x" retry_after=soon')
        with pytest.raises(ScriptError, match="not be negative"):
            plan('error 429 rate_limit_error "x" retry_after=-1')

    def test_stall_is_a_gate_element(self):
        """`stall <gate>` plans like `hold` (an element naming its gate), with its own kind so the server can keep it
        silent.

        Returns:
            None.
        Raises:
            AssertionError: On a wrong plan.
        """
        r = plan('say "half"; stall g1; reply "rest"').responses[0]
        assert kinds(r) == ["text", "stall", "text"] and r.elements[1].name == "g1"
        with pytest.raises(ScriptError, match="needs a text argument"):
            plan("stall")

    def test_size_fills_a_text_or_a_write(self):
        """`size=<n>` puts exactly n characters of numbered filler in a say / think / reply block or a Write's content;
        a text and a size together, a bad size, or one over the cap fail loudly.

        Returns:
            None.
        Raises:
            AssertionError: On a wrong length or a missing refusal.
        """
        r = plan('think size=10; say size=1000; reply size=5').responses[0]
        assert [len(el.text) for el in r.elements] == [10, 1000, 5]
        assert r.elements[1].text.startswith("filler line 000001 ")
        w = plan('write C:/w/big.txt size=200000; reply "ok"').responses[0]
        assert w.elements[0].name == "Write" and len(w.elements[0].input["content"]) == 200000
        assert plan('write C:/w/a.txt "x"; reply "ok"').responses[0].elements[0].input["content"] == "x"
        with pytest.raises(ScriptError, match="not both"):
            plan('say "text" size=10')
        with pytest.raises(ScriptError, match="whole number"):
            plan("say size=lots")
        with pytest.raises(ScriptError, match=r"size must be 0\.\."):
            plan(f"say size={MAX_FILLER_CHARS + 1}")

    def test_filler_is_numbered_lines_of_a_fixed_width(self):
        """The filler is deterministic, exactly as long as asked, and made of numbered lines of FILLER_LINE_CHARS.

        Returns:
            None.
        Raises:
            AssertionError: On a wrong shape.
        """
        text = filler(FILLER_LINE_CHARS * 3 + 7)
        lines = text.split("\n")
        assert len(text) == FILLER_LINE_CHARS * 3 + 7 and text == filler(FILLER_LINE_CHARS * 3 + 7)
        assert [len(x) + 1 for x in lines[:3]] == [FILLER_LINE_CHARS] * 3
        assert lines[2].startswith("filler line 000003 ") and lines[3] == "filler "
        assert filler(0) == "" and filler(-5) == ""

    def test_stream_delay_hold_drop_stop_usage(self):
        """Stream-control steps land in order on the response; stop and usage override its fields.

        Returns:
            None.
        Raises:
            AssertionError: On a wrong plan.
        """
        p = plan('stream 5 50ms; say "hello world"; delay 1.5s; hold g1; drop after=2; stop max_tokens; usage in=5000 cache_read=40000')
        r = p.responses[0]
        assert kinds(r) == ["text", "delay", "hold", "drop"]
        assert (r.elements[0].chunk, r.elements[0].chunk_ms) == (5, 50)
        assert r.elements[1].ms == 1500
        assert r.elements[2].name == "g1"
        assert r.elements[3].after == 2
        assert r.stop == "max_tokens"
        assert r.usage == {"input_tokens": 5000, "cache_read_input_tokens": 40000}

    def test_stop_refusal_carries_stop_details(self):
        """`stop refusal` adds the API's stop_details (defaults, or the scripted category and explanation); other
        stop reasons carry none and take no further argument.

        Returns:
            None.
        Raises:
            AssertionError: On wrong details.
        """
        bare = plan('say "no"; stop refusal').responses[0]
        assert bare.stop == "refusal"
        assert bare.stop_details == {"type": "refusal", "category": "cyber",
                                     "explanation": "This request was blocked by the mock model's scripted refusal."}
        told = plan('say "no"; stop refusal bio "Not this one."').responses[0]
        assert told.stop_details == {"type": "refusal", "category": "bio", "explanation": "Not this one."}
        assert plan('say "x"; stop max_tokens').responses[0].stop_details is None
        with pytest.raises(ScriptError, match="no further argument"):
            plan('say "x"; stop max_tokens bio')
        with pytest.raises(ScriptError, match="at most a category"):
            plan('say "x"; stop refusal bio "a" "b"')

    def test_next_ends_a_response_without_a_tool(self):
        """`next` closes the response (keeping a `stop` override); the steps after it answer the next request; a
        pending expect / error stays with the response that follows; after a tool call it changes nothing.

        Returns:
            None.
        Raises:
            AssertionError: On a wrong grouping.
        """
        p = plan('say "partial"; stop max_tokens; next; hold g; reply "rest"')
        assert [kinds(r) for r in p.responses] == [["text"], ["hold", "text"]]
        assert p.responses[0].stop == "max_tokens" and p.responses[0].final
        assert p.responses[1].stop == "end_turn" and p.responses[1].final
        q = plan('say "no"; stop refusal; next; say "no again"; stop refusal; next; reply "never"')
        assert [r.stop for r in q.responses] == ["refusal", "refusal", "end_turn"]
        r = plan('reply "first stop"; say "unreached"')
        assert len(r.responses) == 1  # reply still ends the script: `next` is what continues a finished turn
        s = plan('say "first stop"; next; error 529 overloaded_error "busy" times=1; next; reply "continued"')
        assert len(s.responses) == 2 and s.responses[1].fail == (529, "overloaded_error", "busy", 1)
        t = plan('bash "echo"; next; reply "done"')
        assert [kinds(x) for x in t.responses] == [["tool_use"], ["text"]]

    def test_bare_number_delay_is_milliseconds(self):
        """A bare number of milliseconds (the spike's form) is accepted.

        Returns:
            None.
        Raises:
            AssertionError: On a wrong duration.
        """
        assert plan("delay 250; reply \"x\"").responses[0].elements[0].ms == 250

    def test_title_and_fast_are_script_level(self):
        """`title` and `fast` set plan fields and add no element.

        Returns:
            None.
        Raises:
            AssertionError: On a wrong plan.
        """
        p = plan('title "Lexer work"; fast; reply "x"')
        assert p.title == "Lexer work" and p.fast
        assert kinds(p.responses[0]) == ["text"]

    def test_use_splices_a_scenario(self):
        """`use @name` splices a scenario's steps; a self-using scenario is refused, not looped forever.

        Returns:
            None.
        Raises:
            AssertionError: On a wrong splice or a missing refusal.
        """
        scenarios = {"two-tools": 'bash "a"; bash "b"', "loop": "use @loop"}
        p = plan('use @two-tools; reply "done"', scenarios=lambda name: scenarios[name])
        assert len(p.responses) == 3
        with pytest.raises(ScriptError, match="deeper than 8"):
            plan("use @loop", scenarios=lambda name: scenarios[name])
        with pytest.raises(ScriptError, match="scenarios folder"):
            plan("use @two-tools")


class TestShorthands:
    """The shorthand steps expand into the tool calls Claude Code expects."""

    def test_write_edit_read(self):
        """write / edit / read build the file tools' inputs (bare paths with backslashes allowed).

        Returns:
            None.
        Raises:
            AssertionError: On a wrong expansion.
        """
        assert expand_shorthand("write", ["C:\\a.txt", "x"]) == ("tool", ["Write", {"file_path": "C:\\a.txt", "content": "x"}])
        assert expand_shorthand("edit", ["a", "o", "n"])[1][1] == {"file_path": "a", "old_string": "o", "new_string": "n"}
        assert expand_shorthand("read", ["a"]) == ("tool", ["Read", {"file_path": "a"}])

    def test_agent_picks_the_offered_subagent_tool(self):
        """The subagent shorthand uses whichever subagent tool the request offers (Agent, else Task).

        Returns:
            None.
        Raises:
            AssertionError: On a wrong tool name.
        """
        assert expand_shorthand("agent", ["p"], {"Agent", "Bash"})[1][0] == "Agent"
        assert expand_shorthand("agent", ["p"], {"Task", "Bash"})[1][0] == "Task"
        assert expand_shorthand("agent", ["p"])[1][0] == "Agent"

    def test_ask(self):
        """`ask` builds one AskUserQuestion question with a short header and the given labels.

        Returns:
            None.
        Raises:
            AssertionError: On a wrong expansion.
        """
        name, (tool, tool_input) = expand_shorthand("ask", ["Which side?", "Left,Right"])
        assert (name, tool) == ("tool", "AskUserQuestion")
        question = tool_input["questions"][0]
        assert question["question"] == "Which side?"
        assert [o["label"] for o in question["options"]] == ["Left", "Right"]
        assert question["multiSelect"] is False and len(question["header"]) <= 12

    def test_ask_needs_two_to_four_options(self):
        """`ask` refuses fewer than two or a missing option list (Claude Code's own limits).

        Returns:
            None.
        Raises:
            AssertionError: When no ScriptError is raised.
        """
        with pytest.raises(ScriptError, match="2 to 4"):
            expand_shorthand("ask", ["Q?", "Only"])
        with pytest.raises(ScriptError, match="needs options"):
            expand_shorthand("ask", ["Q?"])


class TestCheckToolInput:
    """Scripted tool inputs are checked against the request's input_schema (typos fail loudly)."""

    SCHEMA = {"type": "object", "required": ["command"], "properties": {"command": {"type": "string"},
                                                                         "timeout": {"type": "number"}}}

    def test_fine(self):
        """A valid input has no problems.

        Returns:
            None.
        Raises:
            AssertionError: When a problem is reported.
        """
        assert check_tool_input("Bash", {"command": "ls"}, self.SCHEMA) == []

    def test_missing_and_wrong_type(self):
        """A missing required key and a wrong JSON type are both reported.

        Returns:
            None.
        Raises:
            AssertionError: When either is missed.
        """
        problems = check_tool_input("Bash", {"timeout": "5"}, self.SCHEMA)
        assert any("missing required 'command'" in p for p in problems)
        assert any("'timeout' must be number" in p for p in problems)

    def test_tool_not_offered(self):
        """A tool the request does not offer is a problem (Claude Code would reject the call).

        Returns:
            None.
        Raises:
            AssertionError: When it is not reported.
        """
        assert check_tool_input("Nope", {}, None) == ["the request offers no tool named 'Nope'"]

    def test_bool_is_not_a_number(self):
        """A JSON boolean is not accepted where a number is declared (Python's bool is an int).

        Returns:
            None.
        Raises:
            AssertionError: When the boolean passes.
        """
        assert check_tool_input("Bash", {"command": "x", "timeout": True}, self.SCHEMA)


def test_element_defaults():
    """An Element's optional fields default to "nothing to do" values.

    Returns:
        None.
    Raises:
        AssertionError: On a changed default.
    """
    el = Element("text", text="x")
    assert (el.chunk, el.ms, el.after, el.input) == (0, 0, 0, {})
