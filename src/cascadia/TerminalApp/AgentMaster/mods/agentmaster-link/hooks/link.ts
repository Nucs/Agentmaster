/**
 * agentmaster-link: the pure half of the link (no `$`), so its rules are tested
 * on their own. The protocol is Agentmaster's (its SessionLink.h; MOD_LINK.md):
 *
 *   POST /v1/hello     {protocol, mod, claude, session}   -> {protocol, hold}
 *   GET  /v1/commands?hold=<ms>&session=<id>              -> {commands: [...]}
 *        held by Agentmaster until a command is queued or the hold runs out;
 *        the poll is the link's heartbeat
 *   POST /v1/results   {results: [{id, ...}]}             -> {ok, taken}
 *
 * Every request carries the launch's token in `x-am-token`. A 404 means the
 * token is not Agentmaster's (this claude was not launched by the running
 * Agentmaster) and the link stops for good.
 */

/** The protocol this mod speaks; Agentmaster answers 409 to another one. */
export const PROTOCOL = 1

/** This mod's version, sent in the hello (Agentmaster logs it). */
export const VERSION = '0.1.0'

/**
 * How long Agentmaster may hold a poll, in ms. Short on purpose: an open
 * fetch holds a headless claude's exit until it is answered (MOD_LINK.md F9).
 */
export const HOLD_MS = 5000

/** The longest pause between attempts after failures, in ms. */
export const MAX_BACKOFF_MS = 30000

/**
 * How many failures in a row end the loop. Agentmaster's tabs end with
 * Agentmaster, so a link that keeps failing belongs to a claude that is
 * about to end, or to a module environment that was unloaded.
 */
export const MAX_FAILURES = 20

/** How many times a command's result is posted before it is given up. */
export const RESULT_ATTEMPTS = 3

/** The efforts `$.model.fork` takes (ModelEffort). */
export const EFFORTS = ['low', 'medium', 'high', 'xhigh', 'max'] as const

/** One effort level. */
export type Effort = (typeof EFFORTS)[number]

/** One command from Agentmaster, as the poll's answer carries it. */
export type LinkCommand = {
  /** Agentmaster's id for it; the result names it. */
  readonly id: string
  /** What to do: `fork` (a side question over the session's own transcript). */
  readonly kind: string
  /** `fork`: the one user message the fork answers. */
  readonly prompt?: string
  /** `fork`: an effort other than the session's (costs the cache: never sent by /btw). */
  readonly effort?: string
}

/**
 * Whether an endpoint is one this mod may dial: Agentmaster's loopback
 * server, `http://127.0.0.1:<port>`, and nothing else. A value from any other
 * source (a user's environment, a nested shell) is ignored, so the token is
 * never sent anywhere but this machine's Agentmaster.
 *
 * @param endpoint the AM_LINK value
 * @returns true for `http://127.0.0.1:<1..65535>`
 */
export function isLinkEndpoint(endpoint: string | undefined): endpoint is string {
  const m = /^http:\/\/127\.0\.0\.1:(\d{1,5})$/.exec(endpoint ?? '')
  if (!m) return false
  const port = Number(m[1])
  return port >= 1 && port <= 65535
}

/**
 * Whether a token looks like one Agentmaster minted: 32 lowercase hex
 * digits. Anything else is not sent (it could be a stray variable).
 *
 * @param token the AM_LINK_TOKEN value
 * @returns true for a well-formed token
 */
export function isLinkToken(token: string | undefined): token is string {
  return /^[0-9a-f]{32}$/.test(token ?? '')
}

/**
 * The poll's URL: the hold and the conversation this claude runs now (a
 * /clear moves the link to the new id with no message of its own).
 *
 * @param endpoint Agentmaster's endpoint
 * @param session the session id `$.session.id()` gives now
 * @returns the URL
 */
export function pollUrl(endpoint: string, session: string): string {
  return `${endpoint}/v1/commands?hold=${HOLD_MS}&session=${encodeURIComponent(session)}`
}

/**
 * The pause after the given number of failures in a row: 1 s, 2 s, 4 s ...
 * up to MAX_BACKOFF_MS.
 *
 * @param failures failures in a row (1 or more)
 * @returns the pause in ms
 */
export function backoffMs(failures: number): number {
  const steps = Math.min(Math.max(failures, 1) - 1, 5)
  return Math.min(MAX_BACKOFF_MS, 1000 * 2 ** steps)
}

/**
 * Reads a poll's answer: the commands to run, and whether Agentmaster asked
 * the link to stop. A body that is not the expected JSON reads as nothing to
 * do (the next poll asks again).
 *
 * @param text the response body
 * @returns the commands (well-formed ones only) and the release flag
 */
export function readPollAnswer(text: string): { commands: LinkCommand[]; release: boolean } {
  let body: unknown
  try {
    body = JSON.parse(text)
  } catch {
    return { commands: [], release: false }
  }
  if (typeof body !== 'object' || body === null) return { commands: [], release: false }
  const o = body as { commands?: unknown; release?: unknown }
  const commands = Array.isArray(o.commands)
    ? o.commands.filter(
        (c): c is LinkCommand =>
          typeof c === 'object' &&
          c !== null &&
          typeof (c as LinkCommand).id === 'string' &&
          typeof (c as LinkCommand).kind === 'string',
      )
    : []
  return { commands, release: o.release === true }
}

/**
 * The effort a `fork` command asks for, when it is one of the five levels;
 * anything else is dropped (the fork then runs at the session's own level,
 * which is the one that reads the cache).
 *
 * @param effort the command's effort
 * @returns the level, or undefined
 */
export function forkEffort(effort: string | undefined): Effort | undefined {
  return (EFFORTS as readonly string[]).includes(effort ?? '') ? (effort as Effort) : undefined
}
