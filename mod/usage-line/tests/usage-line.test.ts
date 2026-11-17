import type { SessionRateLimit } from 'claude-code'
import { expect, mock, test } from 'claude-code/testing'
import { meterReading, usageText } from '../hooks/usage-text'

const NOW = Date.parse('2026-11-17T10:00:00Z')
const LATER = '2026-11-17T12:00:00Z'
const EARLIER = '2026-11-17T09:00:00Z'
const MINUTE = 60_000

/** A meter file as the taskbar meter writes it, fetched `ageMs` before NOW. */
function meterFile(
  ageMs: number,
  fable: Record<string, unknown> | null = {
    utilization: 41,
    resets_at: LATER,
    name: 'Weekly Fable',
  },
): string {
  const utilization: Record<string, unknown> = {
    five_hour: { utilization: 26, resets_at: LATER, name: '5-hour' },
    seven_day: { utilization: 44, resets_at: LATER, name: 'Weekly' },
  }
  if (fable !== null) utilization['weekly_scoped:Fable'] = fable

  return JSON.stringify({ fetchedAtMs: NOW - ageMs, source: 'live', utilization })
}

// The pure text builder.

test('both windows', async () => {
  const limits: SessionRateLimit[] = [
    { kind: 'seven_day', percentUsed: 43, resetsAt: LATER },
    { kind: 'five_hour', percentUsed: 14, resetsAt: LATER },
  ]

  expect(usageText(limits, NOW)).toBe('5h 14% | W 43%')
})

test('no reading gives no line', async () => {
  expect(usageText([], NOW)).toBe(undefined)
})

test('a fractional percent is floored', async () => {
  const limits: SessionRateLimit[] = [
    { kind: 'five_hour', percentUsed: 23.9 },
    { kind: 'seven_day', percentUsed: 7.5 },
  ]

  expect(usageText(limits, NOW)).toBe('5h 23% | W 7%')
})

test('a spend limit is left out, and alone gives no line', async () => {
  const spend: SessionRateLimit = { kind: 'spend_limit', percentUsed: 120 }

  expect(usageText([spend, { kind: 'seven_day', percentUsed: 43 }], NOW)).toBe(
    'W 43%',
  )
  expect(usageText([spend], NOW)).toBe(undefined)
})

test('a window without a reading is left out', async () => {
  expect(usageText([{ kind: 'five_hour', percentUsed: 14 }], NOW)).toBe('5h 14%')
})

test('a window whose reset time has passed shows 0%', async () => {
  const limits: SessionRateLimit[] = [
    { kind: 'five_hour', percentUsed: 88, resetsAt: EARLIER },
    { kind: 'seven_day', percentUsed: 43, resetsAt: LATER },
  ]

  expect(usageText(limits, NOW)).toBe('5h 0% | W 43%')
})

test('a window with no reset time shows its value as given', async () => {
  expect(usageText([{ kind: 'seven_day', percentUsed: 43 }], NOW)).toBe('W 43%')
})

test('the Fable part comes last, and alone stands as F', async () => {
  const limits: SessionRateLimit[] = [
    { kind: 'five_hour', percentUsed: 23 },
    { kind: 'seven_day', percentUsed: 43 },
  ]

  expect(usageText(limits, NOW, { fable: 41 })).toBe('5h 23% | W 43% | F 41%')
  expect(usageText([], NOW, { fable: 41 })).toBe('F 41%')
  expect(usageText([], NOW, { fable: 0 })).toBe('F 0%')
})

// The meter file as a fallback for the session windows.

test('session empty and a good meter file give all three parts', async () => {
  expect(usageText([], NOW, meterReading(meterFile(5 * MINUTE), NOW))).toBe(
    '5h 26% | W 44% | F 41%',
  )
})

test('a session value wins over the meter value', async () => {
  const limits: SessionRateLimit[] = [
    { kind: 'five_hour', percentUsed: 30, resetsAt: LATER },
    { kind: 'seven_day', percentUsed: 50, resetsAt: LATER },
  ]

  expect(usageText(limits, NOW, meterReading(meterFile(MINUTE), NOW))).toBe(
    '5h 30% | W 50% | F 41%',
  )
})

test('session empty and a stale meter file give no line', async () => {
  expect(
    usageText([], NOW, meterReading(meterFile(30 * MINUTE + 1), NOW)),
  ).toBe(undefined)
})

test('a session with only five_hour takes W from the meter file', async () => {
  const limits: SessionRateLimit[] = [
    { kind: 'five_hour', percentUsed: 12.7, resetsAt: LATER },
  ]

  expect(usageText(limits, NOW, meterReading(meterFile(MINUTE), NOW))).toBe(
    '5h 12% | W 44% | F 41%',
  )
})

// The meter file parser.

test('all three values from a good meter file', async () => {
  expect(meterReading(meterFile(5 * MINUTE), NOW)).toEqual({
    fiveHour: 26,
    sevenDay: 44,
    fable: 41,
  })
})

test('the real meter file of 2026-11-17 parses', async () => {
  const real =
    '{"fetchedAtMs":1794869563338,"source":"live","utilization":{"five_hour":{"utilization":26,"resets_at":"2026-11-17T02:29:59.558240+00:00","name":"5-hour"},"seven_day":{"utilization":44,"resets_at":"2026-11-17T16:00:00.558258+00:00","name":"Weekly"},"weekly_scoped:Fable":{"utilization":41,"resets_at":"2026-11-17T16:00:00.558412+00:00","name":"Weekly Fable"}}}'

  expect(meterReading(real, 1794869563338 + MINUTE)).toEqual({
    fiveHour: 26,
    sevenDay: 44,
    fable: 41,
  })
})

test('no meter file gives no values', async () => {
  expect(meterReading(undefined, NOW)).toEqual({})
})

test('a broken or half-written meter file gives no values', async () => {
  expect(meterReading('{"fetchedAtMs":17948695', NOW)).toEqual({})
  expect(meterReading('', NOW)).toEqual({})
  expect(meterReading('null', NOW)).toEqual({})
  expect(meterReading('[1,2]', NOW)).toEqual({})
})

test('a window the meter file lacks is left out', async () => {
  expect(meterReading(meterFile(MINUTE, null), NOW)).toEqual({
    fiveHour: 26,
    sevenDay: 44,
  })
  expect(
    meterReading(meterFile(MINUTE, { resets_at: LATER, name: 'x' }), NOW).fable,
  ).toBe(undefined)
  expect(
    meterReading(JSON.stringify({ fetchedAtMs: NOW, source: 'live' }), NOW),
  ).toEqual({})
})

test('a meter file older than 30 minutes gives no values', async () => {
  expect(meterReading(meterFile(30 * MINUTE), NOW).fable).toBe(41)
  expect(meterReading(meterFile(30 * MINUTE + 1), NOW)).toEqual({})
  expect(meterReading(JSON.stringify({ utilization: {} }), NOW)).toEqual({})
})

test('a meter window whose reset time has passed shows 0', async () => {
  expect(
    meterReading(meterFile(MINUTE, { utilization: 77, resets_at: EARLIER }), NOW)
      .fable,
  ).toBe(0)
})

test('a fractional meter value is floored, never below 0', async () => {
  expect(
    meterReading(meterFile(MINUTE, { utilization: 41.9, resets_at: LATER }), NOW)
      .fable,
  ).toBe(41)
  expect(meterReading(meterFile(MINUTE, { utilization: -3 }), NOW).fable).toBe(0)
})

// The hooks: start, measure, the 60 s refresh, the status entry and the
// SessionMode labels. One test, since the mod's module state carries on.

test('status entry first, then the SessionMode labels', async ($, on) => {
  const clock = mock.clock(on, { now: NOW })
  mock.env(on, { APPDATA: 'C:\\Users\\someone\\AppData\\Roaming' })
  const statuses: (string | undefined)[] = []
  const modes: (readonly string[])[] = []
  const reads: string[] = []
  let limits: SessionRateLimit[] = [
    { kind: 'five_hour', percentUsed: 23.4, resetsAt: LATER },
    { kind: 'seven_day', percentUsed: 43, resetsAt: LATER },
  ]
  let meter: string | undefined = meterFile(5 * MINUTE)

  on('session.start', (_, e) => ({ cwd: e.cwd }))
  on('session.measure', (_, e) => ({ changed: e.changed }))
  on('session.usage', () => ({
    value: { startedAt: NOW, context: { window: 200_000 }, rateLimits: limits },
  }))
  on('fs.read', (_, e) => {
    reads.push(e.path)
    if (meter === undefined) throw new Error('ENOENT')
    return { value: meter }
  })
  on('ui.status', (_, e) => {
    statuses.push(e.text)
    return { value: undefined }
  })
  // Stands for the engine: records the mode labels as the mod handed them on.
  on('ui.render', ($, e) => {
    modes.push((e.props as { modes: readonly string[] }).modes)
    return h($.ui.resolve(e).Text, {}, 'modes') as never
  })

  // Before any SessionMode draw: the status entry carries the text.
  await $.session.start({ cwd: '/', surface: 'desktop', isInteractive: true })
  expect(reads.at(-1)).toBe(
    'C:\\Users\\someone\\AppData\\Roaming\\ClaudeTaskbarUsage\\last.json',
  )
  expect(statuses.at(-1)).toBe('5h 23% | W 43% | F 41%')

  // The meter file goes missing: no F part, no fault.
  meter = undefined
  await clock.advance(60_000)
  expect(statuses.at(-1)).toBe('5h 23% | W 43%')

  // The first SessionMode draw: the text leads the engine's labels, and the
  // status entry is cleared.
  meter = meterFile(5 * MINUTE)
  await clock.advance(60_000)
  const ui = await $.ui.mount({
    plugin: 'usage-line',
    component: 'SessionMode',
    surface: 'desktop',
    props: { modes: ['focus'] },
  })
  // The mod draws its own Text: the usage text, then the engine's labels.
  const drawnText = async () => (await ui.find({ type: 'Text' }))?.text
  expect(await drawnText()).toBe('5h 23% | W 43% | F 41% & focus')
  expect(statuses.at(-1)).toBe(undefined)
  const statusCount = statuses.length

  // A new reading redraws the labels, with no status entry set again.
  await $.session.measure({
    context: { window: 200_000 },
    rateLimits: [
      { kind: 'five_hour', percentUsed: 30, resetsAt: LATER },
      { kind: 'seven_day', percentUsed: 44, resetsAt: LATER },
    ],
    changed: ['rateLimits'],
  })
  await ui.drawn()
  expect(await drawnText()).toBe('5h 30% | W 44% | F 41% & focus')

  // The timer: a passed reset gives 0%; a stale meter file drops F.
  limits = [
    { kind: 'five_hour', percentUsed: 14, resetsAt: '2026-11-17T10:02:30Z' },
    { kind: 'seven_day', percentUsed: 44, resetsAt: LATER },
  ]
  meter = meterFile(40 * MINUTE)
  await clock.advance(60_000)
  await ui.drawn()
  expect(await drawnText()).toBe('5h 0% | W 44% & focus')

  // The engine's own labels are kept when they change.
  await ui.redraw({ modes: ['focus', 'memory paused'] })
  expect(await drawnText()).toBe('5h 0% | W 44% & focus & memory paused')

  expect(statuses.length).toBe(statusCount)
  await ui.unmount()
})
