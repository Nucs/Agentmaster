import type { Register } from 'claude-code'

/**
 * Waits for one global timer callback, racing it against `$.clock.sleep`, so a timer that never fires
 * still ends the check.
 *
 * @param arm starts the timer and hands it the callback that settles the race
 * @param sleep the engine's own wait, which ends the race when the timer stays silent
 * @returns "fired after N ms", "threw: …", or "did not fire within 1.5 s"
 */
async function race(arm: (done: () => void) => void, sleep: () => Promise<void>): Promise<string> {
  const t0 = performance.now()
  const fired = new Promise<string>(resolve => {
    try {
      arm(() => resolve(`fired after ${Math.round(performance.now() - t0)} ms`))
    } catch (error) {
      resolve(`threw: ${describe(error)}`)
    }
  })
  return Promise.race([fired, sleep().then(() => 'did not fire within 1.5 s')])
}

/**
 * Describes a thrown value without converting it to a primitive, which is what failed in the first run.
 *
 * @param error whatever was thrown
 * @returns its name and message when it has them, else its type
 */
function describe(error: unknown): string {
  const e = error as { name?: unknown; message?: unknown }
  return typeof e?.message === 'string' ? `${typeof e.name === 'string' ? e.name : 'Error'}: ${e.message}` : typeof error
}

/**
 * Runs one step and records its result or the error it threw, so one failing step never hides the rest.
 *
 * @param report the record to fill
 * @param key the step's name in the record
 * @param step the step
 */
async function step(report: Record<string, unknown>, key: string, step: () => unknown | Promise<unknown>): Promise<void> {
  try {
    report[key] = await step()
  } catch (error) {
    report[key] = `threw: ${describe(error)}`
  }
}

/**
 * Checks once the session starts and writes `<AMP_OUT>/libprobe4.json`.
 *
 * @param on the engine's registrar
 */
export const register: Register = on => {
  on('session.start', async ($, e, next) => {
    const started = await next(e)
    const outDir = (await $.env.get('AMP_OUT')) ?? ''
    const report: Record<string, unknown> = {}
    const g = globalThis as Record<string, unknown>
    await step(report, 'setTimeoutType', () => typeof g.setTimeout)
    await step(report, 'setTimeoutKeys', () => Object.getOwnPropertyNames(g.setTimeout as object).slice(0, 10))
    await step(report, 'setTimeoutProtoIsFunction', () => Object.getPrototypeOf(g.setTimeout) === Function.prototype)
    await step(report, 'setTimeoutFires', () => race(done => { (g.setTimeout as (f: () => void, ms: number) => unknown)(done, 50) }, () => $.clock.sleep(1500)))
    await step(report, 'setIntervalFires', () => race(done => {
      let ticks = 0
      const id = (g.setInterval as (f: () => void, ms: number) => unknown)(() => {
        ticks += 1
        if (ticks === 3) { (g.clearInterval as (handle: unknown) => void)(id); done() }
      }, 30)
    }, () => $.clock.sleep(1500)))
    await step(report, 'promiseThen', () => Promise.resolve('then() runs'))
    await $.fs.write(`${outDir}/libprobe4.json`, JSON.stringify(report, null, 2))
    return started
  })
}
