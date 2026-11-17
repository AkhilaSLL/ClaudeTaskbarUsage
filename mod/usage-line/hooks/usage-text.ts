import type { SessionRateLimit } from 'claude-code'

/**
 * The percents read from the taskbar meter's `last.json`: each absent when
 * the file has no usable reading for that window.
 */
export type MeterReading = {
  readonly fiveHour?: number
  readonly sevenDay?: number
  readonly fable?: number
}

// The session windows shown, in this order, with the meter field that stands
// in while the session has no reading for the window (after a restart, until
// the first API response). `SessionRateLimit.kind` names only `five_hour`,
// `seven_day` and a gateway's `spend_limit`: no field tells a Fable-scoped
// weekly window apart, so `F` comes from the meter file alone.
const SHOWN: ReadonlyArray<
  readonly [kind: string, label: string, fallback: 'fiveHour' | 'sevenDay']
> = [
  ['five_hour', '5h', 'fiveHour'],
  ['seven_day', 'W', 'sevenDay'],
]

/** The keys of the windows in the meter file's `utilization`. */
const METER_KEYS = {
  fiveHour: 'five_hour',
  sevenDay: 'seven_day',
  fable: 'weekly_scoped:Fable',
} as const

/** A meter reading older than this is not shown. */
export const METER_MAX_AGE_MS = 30 * 60_000

/**
 * The footer text for the given rate-limit windows at `nowMs` (milliseconds
 * since the epoch): `5h 14% | W 43% | F 41%`, or undefined when no part has a
 * reading. A session window without a reading takes the meter's value; `F`
 * is the meter's alone.
 */
export function usageText(
  limits: readonly SessionRateLimit[],
  nowMs: number,
  meter: MeterReading = {},
): string | undefined {
  const parts: string[] = []

  for (const [kind, label, fallback] of SHOWN) {
    const limit = limits.find(l => l.kind === kind)

    if (limit !== undefined && Number.isFinite(limit.percentUsed)) {
      parts.push(`${label} ${percentAt(limit.percentUsed, limit.resetsAt, nowMs)}%`)
    } else if (meter[fallback] !== undefined) {
      parts.push(`${label} ${meter[fallback]}%`)
    }
  }

  if (meter.fable !== undefined) parts.push(`F ${meter.fable}%`)

  return parts.length > 0 ? parts.join(' | ') : undefined
}

/**
 * The 5-hour, weekly and Fable weekly percents from the text of the taskbar
 * meter's `last.json` at `nowMs`. Nothing when the text is missing, not JSON,
 * or was fetched more than 30 minutes before `nowMs`; a window the file lacks
 * or holds no number for is left out.
 */
export function meterReading(
  fileText: string | undefined,
  nowMs: number,
): MeterReading {
  if (fileText === undefined) return {}

  let data: unknown
  try {
    data = JSON.parse(fileText)
  } catch {
    return {}
  }

  if (!isRecord(data)) return {}

  const fetchedAtMs = data['fetchedAtMs']
  if (typeof fetchedAtMs !== 'number' || !Number.isFinite(fetchedAtMs)) {
    return {}
  }
  if (nowMs - fetchedAtMs > METER_MAX_AGE_MS) return {}

  const windows = data['utilization']
  if (!isRecord(windows)) return {}

  const reading: { fiveHour?: number; sevenDay?: number; fable?: number } = {}

  for (const field of ['fiveHour', 'sevenDay', 'fable'] as const) {
    const value = windowPercent(windows[METER_KEYS[field]], nowMs)
    if (value !== undefined) reading[field] = value
  }

  return reading
}

function windowPercent(window: unknown, nowMs: number): number | undefined {
  if (!isRecord(window)) return undefined

  const used = window['utilization']
  if (typeof used !== 'number' || !Number.isFinite(used)) return undefined

  const resetsAt = window['resets_at']

  return percentAt(
    used,
    typeof resetsAt === 'string' ? resetsAt : undefined,
    nowMs,
  )
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value)
}

function percentAt(
  used: number,
  resetsAt: string | undefined,
  nowMs: number,
): number {
  const resetMs = resetsAt === undefined ? Number.NaN : Date.parse(resetsAt)

  if (Number.isFinite(resetMs) && resetMs <= nowMs) return 0

  return Math.max(0, Math.floor(used))
}
