import type { EngineInterface, Register } from 'claude-code'

/**
 * Where the probe server listens, read from the environment the probe run sets. Empty strings mean
 * "that transport is not under test"; every probe call is skipped for it.
 */
type Probe = {
  /** `http://127.0.0.1:<port>`: plain TCP on the loopback. */
  tcp: string
  /** The absolute path of the AF_UNIX socket, for `socketPath`. */
  sock: string
  /** `\\.\pipe\<name>`: a named pipe tried through the same `socketPath` field. */
  pipe: string
}

/** What one probe fetch came to: the answer and its round trip, or the error text. */
type Attempt =
  | { ok: true; status: number; text: string; ms: number }
  | { ok: false; error: string; ms: number }

/**
 * Reads the probe's three endpoints. `$.env.get` takes literal names only (the loader reads them off
 * the source), so each is spelled out.
 *
 * @param $ the engine interface
 * @returns the endpoints; a missing variable reads as ""
 */
async function readProbe($: EngineInterface): Promise<Probe> {
  return {
    tcp: (await $.env.get('AMP_TCP')) ?? '',
    sock: (await $.env.get('AMP_SOCK')) ?? '',
    pipe: (await $.env.get('AMP_PIPE')) ?? '',
  }
}

/**
 * Runs one fetch and times it, never throwing: the probe's whole point is to record failures.
 *
 * @param $ the engine interface
 * @param url the URL; with `socketPath`, its host is only the Host header
 * @param socketPath an AF_UNIX path or a pipe name to go over instead of TCP; "" for plain TCP
 * @returns the status and the first 200 characters of the body, or the error's text, with the ms taken
 */
async function attempt($: EngineInterface, url: string, socketPath: string): Promise<Attempt> {
  const t0 = await $.clock.now()
  try {
    const res = socketPath === '' ? await $.http.fetch(url) : await $.http.fetch(url, { socketPath })
    return { ok: true, status: res.status, text: res.text.slice(0, 200), ms: (await $.clock.now()) - t0 }
  } catch (error) {
    return { ok: false, error: String(error).slice(0, 400), ms: (await $.clock.now()) - t0 }
  }
}

/**
 * Posts one finding to the probe server over TCP (the transport assumed to work); a failure lands in
 * the debug log only, since there is nowhere else to send it.
 *
 * @param $ the engine interface
 * @param probe the endpoints
 * @param what a short label for the server log
 * @param data any JSON-serializable finding
 */
async function report($: EngineInterface, probe: Probe, what: string, data: unknown): Promise<void> {
  try {
    await $.http.fetch(`${probe.tcp}/report?what=${encodeURIComponent(what)}`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify(data),
    })
  } catch (error) {
    $.ui.log(`amprobe: report ${what} failed: ${String(error)}`, { to: 'debug' })
  }
}

/**
 * A loop started from `session.start` and left running after that hook returned: does a `$` call
 * still work outside any dispatch, and does `$.clock.sleep` work there?
 *
 * @param $ the engine interface
 * @param probe the endpoints
 */
async function tickLoop($: EngineInterface, probe: Probe): Promise<void> {
  for (let i = 0; i < 60; i++) {
    const r = await attempt($, `${probe.tcp}/tick?i=${i}`, '')
    if (!r.ok) {
      await report($, probe, 'tick-failed', { i, r })
      return
    }
    try {
      await $.clock.sleep(300)
    } catch (error) {
      await report($, probe, 'tick-sleep-failed', { i, error: String(error) })
      return
    }
  }
}

/**
 * Registers the probe's observers. Every hook passes `next(e)` through unchanged: the probe watches,
 * it never changes what the session does.
 *
 * @param on the engine's registrar
 */
export const register: Register = on => {
  on('session.start', async ($, e, next) => {
    const started = await next(e)
    const probe = await readProbe($)
    const results: Record<string, unknown> = {}
    results.tcp = await attempt($, `${probe.tcp}/hello?via=tcp`, '')
    results.tcpWarm = await attempt($, `${probe.tcp}/hello?via=tcp-warm`, '')
    if (probe.sock !== '') results.unix = await attempt($, 'http://localhost/hello?via=unix', probe.sock)
    if (probe.pipe !== '') results.pipe = await attempt($, 'http://localhost/hello?via=pipe', probe.pipe)
    // A fetch in flight does not count against the hook's 10 s budget: a 12 s long-poll held
    // inside session.start shows whether the host itself cuts a slow answer.
    results.poll12s = await attempt($, `${probe.tcp}/poll?ms=12000`, '')
    results.session = {
      id: await $.session.id(),
      surface: e.surface,
      isInteractive: e.isInteractive,
      version: await $.session.version(),
      wt: (await $.env.get('WT_SESSION')) ?? null,
    }
    await report($, probe, 'session.start', results)
    void tickLoop($, probe)
    return started
  })

  on('prompt.submit', async ($, e, next) => {
    const probe = await readProbe($)
    void report($, probe, 'prompt.submit', { origin: e.origin, chars: e.text.length, wait: e.wait })
    return next(e)
  })

  on('turn.start', async ($, e, next) => {
    const probe = await readProbe($)
    void report($, probe, 'turn.start', { turnId: e.turnId })
    return next(e)
  })

  on('turn.complete', async ($, e, next) => {
    const probe = await readProbe($)
    void report($, probe, 'turn.complete', {
      reason: e.reason,
      isAborted: e.isAborted,
      durationMs: e.durationMs,
      agentId: e.agentId ?? null,
      usage: e.usage ?? null,
    })
    return next(e)
  })

  on('classic.Stop', async ($, e, next) => {
    const probe = await readProbe($)
    void report($, probe, 'classic.Stop', {
      backgroundTasks: e.background_tasks ?? null,
      sessionCrons: e.session_crons ?? null,
      lastMessageHead: (e.last_assistant_message ?? '').slice(0, 60),
    })
    return next(e)
  })

  on('session.end', async ($, e, next) => {
    const probe = await readProbe($)
    await report($, probe, 'session.end', { reason: e.reason, budgetMs: next.budget.ms, remainingMs: next.budget.remainingMs })
    return next(e)
  })
}
