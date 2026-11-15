import type { HttpInit, HttpResponse, On } from 'claude-code'
import type { MockClock } from 'claude-code/testing'
import { expect, mock, test } from 'claude-code/testing'

/** The endpoint and token Agentmaster gives a launch (well-formed). */
const ENDPOINT = 'http://127.0.0.1:52011'
const TOKEN = '0123456789abcdef0123456789abcdef'

/** The conversation the test session runs. */
const SESSION_ID = 'sess-1'

/** One request the mod made, as the test's http.fetch hook saw it. */
type Sent = { url: string; init?: HttpInit }

/** What a test world records. */
type World = {
  /** Every request, in order. */
  sent: Sent[]
  /** Every prompt `$.model.fork` was asked. */
  forks: string[]
  /** The mocked clock (the mod's backoff sleeps). */
  clock: MockClock
}

/**
 * An HTTP answer as `$.http.fetch` resolves it.
 *
 * @param status the status
 * @param body the body (JSON text)
 * @returns the response
 */
function answer(status: number, body: string): HttpResponse {
  return { status, ok: status >= 200 && status < 300, headers: {}, text: body }
}

/**
 * Seats the engine beneath the mod: the environment, the session, the clock,
 * a fork that answers, and an Agentmaster whose answers `route` chooses.
 *
 * @param on the test's hook registrar
 * @param env the variables set
 * @param route answers each request, by its URL and its index among them
 * @returns the world the hooks fill in
 */
function seat(on: On, env: Record<string, string>, route: (url: string, index: number, init?: HttpInit) => HttpResponse): World {
  const world: World = { sent: [], forks: [], clock: mock.clock(on) }
  on('env.get', ($, e) => ({ value: env[e.name] }))
  on('session.id', () => ({ value: SESSION_ID }))
  on('session.version', () => ({ value: { version: '2.1.322', base: '2.1.322', builtAt: '2026-11-12T00:00:00Z' } }))
  on('session.start', ($, e) => ({ cwd: e.cwd }))
  on('model.fork', ($, e) => {
    world.forks.push(e.prompt)
    return {
      value: {
        isAnswered: true,
        text: 'The cache served it.',
        usage: { input_tokens: 425, output_tokens: 61, cache_read_input_tokens: 33010, cache_creation_input_tokens: 122 },
        effort: 'low',
      },
    }
  })
  on('http.fetch', ($, e) => {
    const index = world.sent.length
    world.sent.push({ url: e.url, init: e.init })
    return { value: route(e.url, index, e.init) }
  })
  return world
}

test('a session Agentmaster did not launch makes no request', async ($, on) => {
  const world = seat(on, {}, () => answer(200, '{}'))
  await $.session.start({ cwd: 'C:/proj', surface: 'terminal', isInteractive: true })
  await world.clock.settle()
  expect(world.sent).toEqual([])
})

test('an endpoint that is not loopback is never dialled', async ($, on) => {
  const world = seat(on, { AM_LINK: 'http://example.com:80', AM_LINK_TOKEN: TOKEN }, () => answer(200, '{}'))
  await $.session.start({ cwd: 'C:/proj', surface: 'terminal', isInteractive: true })
  await world.clock.settle()
  expect(world.sent).toEqual([])
})

test('hello, a held poll, a fork run and its result posted, then the release', async ($, on) => {
  const world = seat(on, { AM_LINK: ENDPOINT, AM_LINK_TOKEN: TOKEN }, (url, index) => {
    if (url.endsWith('/v1/hello')) return answer(200, '{"protocol":1,"hold":5000}')
    if (url.endsWith('/v1/results')) return answer(200, '{"ok":true,"taken":1}')
    // The first poll carries the command, the next one releases the link (the test's way to end the loop).
    return index === 1
      ? answer(200, '{"commands":[{"id":"c1","kind":"fork","prompt":"Why the cache?"}]}')
      : answer(200, '{"commands":[],"release":true}')
  })
  await $.session.start({ cwd: 'C:/proj', surface: 'terminal', isInteractive: true })
  await world.clock.settle()

  const hello = world.sent[0]
  expect(hello.url).toBe(`${ENDPOINT}/v1/hello`)
  expect(hello.init?.method).toBe('POST')
  expect(hello.init?.headers?.['x-am-token']).toBe(TOKEN)
  expect(JSON.parse(String(hello.init?.body))).toEqual({ protocol: 1, mod: '0.1.0', claude: '2.1.322', session: SESSION_ID })

  expect(world.sent[1].url).toBe(`${ENDPOINT}/v1/commands?hold=5000&session=${SESSION_ID}`)
  expect(world.sent[1].init?.headers?.['x-am-token']).toBe(TOKEN)
  expect(world.forks).toEqual(['Why the cache?'])

  const posted = world.sent.find(s => s.url.endsWith('/v1/results'))
  const result = JSON.parse(String(posted?.init?.body)).results[0]
  expect(result.id).toBe('c1')
  expect(result.isAnswered).toBe(true)
  expect(result.text).toBe('The cache served it.')
  expect(result.usage.cache_read_input_tokens).toBe(33010)
  expect(result.effort).toBe('low')
})

test('an unknown token stops the link at its hello', async ($, on) => {
  const world = seat(on, { AM_LINK: ENDPOINT, AM_LINK_TOKEN: TOKEN }, () => answer(404, '{"error":"unknown token"}'))
  await $.session.start({ cwd: 'C:/proj', surface: 'terminal', isInteractive: true })
  await world.clock.settle()
  expect(world.sent.map(s => s.url)).toEqual([`${ENDPOINT}/v1/hello`])
})

test('a command the mod does not know is answered, never dropped', async ($, on) => {
  const world = seat(on, { AM_LINK: ENDPOINT, AM_LINK_TOKEN: TOKEN }, (url, index) => {
    if (url.endsWith('/v1/hello')) return answer(200, '{"protocol":1,"hold":5000}')
    if (url.endsWith('/v1/results')) return answer(200, '{"ok":true,"taken":1}')
    return index === 1 ? answer(200, '{"commands":[{"id":"c9","kind":"teleport"}]}') : answer(200, '{"commands":[],"release":true}')
  })
  await $.session.start({ cwd: 'C:/proj', surface: 'terminal', isInteractive: true })
  await world.clock.settle()
  const posted = world.sent.find(s => s.url.endsWith('/v1/results'))
  const result = JSON.parse(String(posted?.init?.body)).results[0]
  expect(result).toEqual(expect.objectContaining({ id: 'c9', isAnswered: false, reason: 'unknown-command' }))
  expect(world.forks).toEqual([])
})
