import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { useToast, toasts } from './useToast'

describe('useToast', () => {
  beforeEach(() => {
    vi.useFakeTimers()
    toasts.value = []
  })
  afterEach(() => vi.useRealTimers())

  it('adds toasts with their kind and auto-dismisses after 4s', () => {
    const t = useToast()
    t.success('saved')
    t.error('boom')
    expect(toasts.value.map((x) => x.kind)).toEqual(['success', 'error'])
    vi.advanceTimersByTime(3999)
    expect(toasts.value).toHaveLength(2)
    vi.advanceTimersByTime(1)
    expect(toasts.value).toHaveLength(0)
  })
})
