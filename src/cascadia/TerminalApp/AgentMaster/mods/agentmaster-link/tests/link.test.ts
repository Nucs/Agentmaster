import { describe, expect, test } from 'claude-code/testing'

import {
  HOLD_MS,
  MAX_BACKOFF_MS,
  backoffMs,
  forkEffort,
  isLinkEndpoint,
  isLinkToken,
  pollUrl,
  readPollAnswer,
} from '../hooks/link'

describe('the endpoint and the token', () => {
  test('only loopback http with a port is dialled', async () => {
    expect(isLinkEndpoint('http://127.0.0.1:52011')).toBe(true)
    expect(isLinkEndpoint('http://127.0.0.1:1')).toBe(true)
    expect(isLinkEndpoint('http://127.0.0.1:65535')).toBe(true)
    // Never another host, scheme, port or a path: the token goes nowhere but this machine's Agentmaster.
    expect(isLinkEndpoint('http://127.0.0.1:0')).toBe(false)
    expect(isLinkEndpoint('http://127.0.0.1:65536')).toBe(false)
    expect(isLinkEndpoint('https://127.0.0.1:52011')).toBe(false)
    expect(isLinkEndpoint('http://localhost:52011')).toBe(false)
    expect(isLinkEndpoint('http://example.com:80')).toBe(false)
    expect(isLinkEndpoint('http://127.0.0.1:52011/v1')).toBe(false)
    expect(isLinkEndpoint('http://127.0.0.1')).toBe(false)
    expect(isLinkEndpoint('')).toBe(false)
    expect(isLinkEndpoint(undefined)).toBe(false)
  })

  test('a token is 32 lowercase hex digits', async () => {
    expect(isLinkToken('0123456789abcdef0123456789abcdef')).toBe(true)
    expect(isLinkToken('0123456789ABCDEF0123456789abcdef')).toBe(false)
    expect(isLinkToken('0123456789abcdef0123456789abcde')).toBe(false)
    expect(isLinkToken('0123456789abcdef0123456789abcdef0')).toBe(false)
    expect(isLinkToken('')).toBe(false)
    expect(isLinkToken(undefined)).toBe(false)
  })
})

describe('the poll', () => {
  test('names the hold and the conversation, encoded', async () => {
    expect(pollUrl('http://127.0.0.1:5', 'a1b2-c3')).toBe(`http://127.0.0.1:5/v1/commands?hold=${HOLD_MS}&session=a1b2-c3`)
    expect(pollUrl('http://127.0.0.1:5', 'a b&c')).toBe(`http://127.0.0.1:5/v1/commands?hold=${HOLD_MS}&session=a%20b%26c`)
  })

  test('the answer gives its well-formed commands and the release', async () => {
    expect(readPollAnswer('{"commands":[{"id":"c1","kind":"fork","prompt":"why?"}]}')).toEqual({
      commands: [{ id: 'c1', kind: 'fork', prompt: 'why?' }],
      release: false,
    })
    expect(readPollAnswer('{"commands":[{"id":1,"kind":"fork"},{"kind":"fork"},"x",{"id":"c2","kind":"fork"}]}').commands).toEqual([
      { id: 'c2', kind: 'fork' },
    ])
    expect(readPollAnswer('{"commands":[],"release":true}')).toEqual({ commands: [], release: true })
    // Not JSON, not an object, no list: nothing to do (the next poll asks again).
    expect(readPollAnswer('not json')).toEqual({ commands: [], release: false })
    expect(readPollAnswer('null')).toEqual({ commands: [], release: false })
    expect(readPollAnswer('{"commands":"x"}')).toEqual({ commands: [], release: false })
  })
})

describe('the backoff and the effort', () => {
  test('doubles from a second up to its cap', async () => {
    expect(backoffMs(1)).toBe(1000)
    expect(backoffMs(2)).toBe(2000)
    expect(backoffMs(3)).toBe(4000)
    expect(backoffMs(6)).toBe(MAX_BACKOFF_MS)
    expect(backoffMs(50)).toBe(MAX_BACKOFF_MS)
    expect(backoffMs(0)).toBe(1000)
  })

  test('only the five levels reach the fork', async () => {
    expect(forkEffort('low')).toBe('low')
    expect(forkEffort('max')).toBe('max')
    expect(forkEffort('xhigh')).toBe('xhigh')
    expect(forkEffort('ultra')).toBe(undefined)
    expect(forkEffort('')).toBe(undefined)
    expect(forkEffort(undefined)).toBe(undefined)
  })
})
