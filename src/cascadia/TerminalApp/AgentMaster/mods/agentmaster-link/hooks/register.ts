import type { EngineInterface, Register } from 'claude-code'

import {
  HOLD_MS,
  MAX_FAILURES,
  PROTOCOL,
  RESULT_ATTEMPTS,
  VERSION,
  backoffMs,
  forkEffort,
  isLinkEndpoint,
  isLinkToken,
  pollUrl,
  readPollAnswer,
  type LinkCommand,
} from './link'

/**
 * agentmaster-link: the session's end of Agentmaster's session link
 * (Agentmaster's MOD_LINK.md, its first slice).
 *
 * Agentmaster launches every Claude tab with AM_LINK (its loopback server,
 * `http://127.0.0.1:<port>`) and AM_LINK_TOKEN (a token for that launch).
 * When both are there, this mod says hello and then keeps one poll open,
 * held by Agentmaster for up to five seconds: the moment Agentmaster has
 * something for this session, the poll answers with it.
 *
 * The one command so far is `fork`: a side question (Telegram's /btw). The mod
 * runs it with `$.model.fork`, the call Claude Code's own /btw is built on:
 * the main thread's last request again with one more user message, every tool
 * denied, its tail never cached and nothing written to the transcript, so the
 * API serves the whole conversation from the prompt cache. The result goes
 * back to Agentmaster as it is.
 *
 * Without the two variables (a session Agentmaster did not launch) the mod
 * does nothing at all. It draws nothing, hooks no tool and no keystroke, and
 * never touches the conversation.
 */

/** One poll loop per module instance (a second session.start must not start another). */
let running = false

/**
 * Hooks `session.start` only: after the engine's own start (`next(e)`), it
 * starts the link's loop once, detached, and answers what the engine answered.
 * No other event enters the mod, so it costs nothing per turn, per tool call
 * or per keystroke.
 *
 * @param on the engine's hook registrar
 */
export const register: Register = on => {
  on('session.start', async ($, e, next) => {
    const started = await next(e)
    if (!running) {
      running = true
      // Detached: session.start is awaited before the first prompt, and the
      // loop lives as long as the session (MOD_LINK.md F4, F5).
      void linkLoop($).finally(() => {
        running = false
      })
    }
    return started
  })
}

/**
 * The link's loop: hello, then one held poll after another, each command run
 * on its own so the polling (the link's heartbeat) never waits for a fork.
 * Ends for good on a 404 or a 409 (not Agentmaster's token, another
 * protocol), on Agentmaster's `release`, or after MAX_FAILURES failures in a
 * row.
 *
 * @param $ the engine interface of the hook that started it
 */
async function linkLoop($: EngineInterface): Promise<void> {
  const endpoint = await $.env.get('AM_LINK')
  const token = await $.env.get('AM_LINK_TOKEN')
  if (!isLinkEndpoint(endpoint) || !isLinkToken(token)) return
  const headers = { 'x-am-token': token, 'content-type': 'application/json' }
  let saidHello = false
  let failures = 0
  for (;;) {
    try {
      const session = await $.session.id()
      if (!saidHello) {
        const { version } = await $.session.version()
        const hello = await $.http.fetch(`${endpoint}/v1/hello`, {
          method: 'POST',
          headers,
          body: JSON.stringify({ protocol: PROTOCOL, mod: VERSION, claude: version, session }),
        })
        if (hello.status === 404 || hello.status === 409) return
        if (!hello.ok) throw new Error(`hello answered ${hello.status}`)
        saidHello = true
      }
      const poll = await $.http.fetch(pollUrl(endpoint, session), { headers: { 'x-am-token': token } })
      if (poll.status === 404) return
      if (!poll.ok) throw new Error(`poll answered ${poll.status}`)
      failures = 0
      const { commands, release } = readPollAnswer(poll.text)
      if (release) return
      for (const command of commands) void runCommand($, endpoint, headers, command)
    } catch {
      failures += 1
      if (failures >= MAX_FAILURES) return
      // Say hello again after a failure: Agentmaster then logs the link anew.
      saidHello = false
      try {
        await $.clock.sleep(backoffMs(failures))
      } catch {
        return // the module's environment is gone
      }
    }
  }
}

/**
 * Runs one command and posts its result. `fork` resolves to `$.model.fork`'s
 * own result, sent as it is (`isAnswered` and `text`, or `reason`, with
 * `usage` and `effort`); anything that goes wrong becomes a result too, so
 * Agentmaster's wait always ends.
 *
 * @param $ the engine interface
 * @param endpoint Agentmaster's endpoint
 * @param headers the token and the content type
 * @param command the command
 */
async function runCommand(
  $: EngineInterface,
  endpoint: string,
  headers: Record<string, string>,
  command: LinkCommand,
): Promise<void> {
  const startedAt = Date.now()
  let result: Record<string, unknown>
  try {
    if (command.kind === 'fork' && typeof command.prompt === 'string' && command.prompt !== '') {
      const effort = forkEffort(command.effort)
      const forked = await $.model.fork(effort === undefined ? { prompt: command.prompt } : { prompt: command.prompt, effort })
      result = { ...forked }
    } else {
      result = { isAnswered: false, reason: 'unknown-command', error: `no such command: ${command.kind}` }
    }
  } catch (error) {
    result = { isAnswered: false, reason: 'mod-error', error: error instanceof Error ? error.message : String(error) }
  }
  const body = JSON.stringify({ results: [{ ...result, id: command.id, ms: Date.now() - startedAt, holdMs: HOLD_MS }] })
  for (let attempt = 1; attempt <= RESULT_ATTEMPTS; attempt += 1) {
    try {
      const posted = await $.http.fetch(`${endpoint}/v1/results`, { method: 'POST', headers, body })
      if (posted.ok || posted.status === 404) return
    } catch {
      // A failed post is tried again; Agentmaster's wait ends on its own timeout otherwise.
    }
    try {
      await $.clock.sleep(1000 * attempt)
    } catch {
      return
    }
  }
}
