import { describe, it, expect, vi, afterEach } from 'vitest'
import { SovdClient, SovdError } from './sovdClient'

const res = (body: unknown, status = 200) =>
  new Response(typeof body === 'string' ? body : JSON.stringify(body), { status })

function mockFetch(...responses: Response[]) {
  const f = vi.fn()
  responses.forEach((r) => f.mockResolvedValueOnce(r))
  vi.stubGlobal('fetch', f)
  return f
}

afterEach(() => vi.unstubAllGlobals())

describe('SovdClient', () => {
  it('discovers the api version from the root', async () => {
    const f = mockFetch(res({ api_versions: ['v2'] }), res({ items: [] }))
    await new SovdClient('http://x').listEntities()
    expect(f.mock.calls[1][0]).toBe('http://x/v2/entities')
  })

  it('sends bearer token only when set', async () => {
    const f = mockFetch(res({}), res({ items: [] }), res({ items: [] }))
    const c = new SovdClient('http://x')
    await c.listEntities()
    expect(f.mock.calls[1][1].headers.Authorization).toBeUndefined()
    c.token = 't'
    await c.listEntities()
    expect(f.mock.calls[2][1].headers.Authorization).toBe('Bearer t')
  })

  it('throws SovdError with parsed code on failure, no retry on 423', async () => {
    const f = mockFetch(res({}), res({ error: 'LOCKED', message: 'held' }, 423))
    const err = await new SovdClient('http://x').getFaults('a').catch((e) => e)
    expect(err).toBeInstanceOf(SovdError)
    expect(err.status).toBe(423)
    expect(err.code).toBe('LOCKED')
    expect(f).toHaveBeenCalledTimes(2)
  })

  it('retries 503 then succeeds', async () => {
    const f = mockFetch(res({}), res('', 503), res({ faults: [{ code: 'P1', status: 'pending' }] }))
    const faults = await new SovdClient('http://x').getFaults('a', 'pending')
    expect(faults).toHaveLength(1)
    expect(f.mock.calls[2][0]).toContain('?status=pending')
  })
})
