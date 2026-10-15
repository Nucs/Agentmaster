import type { EngineInterface, Register } from 'claude-code'

/**
 * Posts one event to the probe server and swallows any failure: a link must never break a turn.
 *
 * @param $ the engine interface
 * @param base `http://127.0.0.1:<port>`, or "" when no server is configured (nothing is sent)
 * @param kind the event's name
 * @param data the event's fields
 */
async function post($: EngineInterface, base: string, kind: string, data: unknown): Promise<void> {
  if (base === '') return
  try {
    await $.http.fetch(`${base}/report?what=${encodeURIComponent(kind)}`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify(data),
    })
  } catch {
    // Observe-only probe: a lost event is acceptable, a thrown one would only be logged and skipped.
  }
}

/**
 * The downstream channel a real link would keep open: one long-poll at a time, re-issued as soon as
 * it answers, with a 1 s pause after a failure so a dead server costs one request a second.
 *
 * @param $ the engine interface
 * @param base the probe server's base URL
 */
async function pollLoop($: EngineInterface, base: string): Promise<void> {
  for (;;) {
    try {
      await $.http.fetch(`${base}/poll?ms=25000`)
    } catch {
      await $.clock.sleep(1000)
    }
  }
}

/**
 * Registers what a session link would hook: the session's start and end, prompts, turns and the
 * Stop payload, each forwarded as one small POST, plus the long-poll loop.
 *
 * @param on the engine's registrar
 */
export const register: Register = on => {
  on('session.start', async ($, e, next) => {
    const started = await next(e)
    const base = (await $.env.get('AMP_TCP')) ?? ''
    void post($, base, 'hello', { id: await $.session.id(), cwd: e.cwd })
    if (base !== '') void pollLoop($, base)
    return started
  })
  on('prompt.submit', async ($, e, next) => {
    void post($, (await $.env.get('AMP_TCP')) ?? '', 'prompt', { origin: e.origin.kind, chars: e.text.length })
    return next(e)
  })
  on('turn.start', async ($, e, next) => {
    void post($, (await $.env.get('AMP_TCP')) ?? '', 'turn.start', { turnId: e.turnId })
    return next(e)
  })
  on('turn.complete', async ($, e, next) => {
    void post($, (await $.env.get('AMP_TCP')) ?? '', 'turn.complete', { reason: e.reason, ms: e.durationMs })
    return next(e)
  })
  on('classic.Stop', async ($, e, next) => {
    void post($, (await $.env.get('AMP_TCP')) ?? '', 'stop', { bg: (e.background_tasks ?? []).length })
    return next(e)
  })
  on('session.end', async ($, e, next) => {
    await post($, (await $.env.get('AMP_TCP')) ?? '', 'bye', { reason: e.reason })
    return next(e)
  })
}
