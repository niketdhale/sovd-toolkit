// Phase 7, D3 (settled 2026-08-20): browser lock lifecycle. A closed tab or
// crashed page has no RAII destructor the way client/lock_guard.hpp's
// LockGuard does, so the mechanism is: a short TTL (10s, not the CLI's 60s
// default) kept alive by a JS heartbeat at ~ttl/2, plus a best-effort
// `beforeunload` release that won't always fire -- that's exactly why the
// TTL has to stay short rather than being relied on.
import { onUnmounted, ref } from 'vue'
import type { SovdClient } from '../api/sovdClient'

const LOCK_TTL_SECONDS = 10

// getPath is a getter, not a static string: the entity a caller like
// DataTable is locking can change (user selects a different entity) across
// this composable's lifetime, but Vue's onUnmounted can only be registered
// once per component -- so useLock() itself is called once at setup(), and
// always operates on whatever getPath() returns *now*. Callers must
// release() before the underlying path changes (DataTable enforces this
// with a watcher); this composable doesn't try to detect that on its own.
export function useLock(client: SovdClient, getPath: () => string) {
  const lockId = ref<string | null>(null)
  let heartbeatTimer: number | undefined

  function stopHeartbeat() {
    if (heartbeatTimer !== undefined) {
      window.clearInterval(heartbeatTimer)
      heartbeatTimer = undefined
    }
  }

  function releaseBestEffort() {
    stopHeartbeat()
    if (lockId.value) {
      client.releaseLockBestEffort(getPath(), lockId.value)
      lockId.value = null
    }
  }

  async function acquire() {
    const path = getPath()
    lockId.value = await client.acquireLock(path, LOCK_TTL_SECONDS)
    heartbeatTimer = window.setInterval(() => {
      if (!lockId.value) return
      // Best-effort, matching LockGuard's C++ heartbeat: a single missed
      // renew doesn't abandon the lock (could be a transient blip) -- the
      // next write attempt surfaces a genuinely lost lock via 423 on its
      // own, which is the right place to fail loudly, not this timer.
      client.renewLock(path, lockId.value, LOCK_TTL_SECONDS).catch(() => {})
    }, (LOCK_TTL_SECONDS * 1000) / 2)
  }

  async function release() {
    stopHeartbeat()
    const id = lockId.value
    lockId.value = null
    if (id) {
      try {
        await client.releaseLock(getPath(), id)
      } catch {
        // best-effort: a release that fails (e.g. already expired
        // server-side) leaves nothing worse than an expired lock already implies
      }
    }
  }

  window.addEventListener('beforeunload', releaseBestEffort)
  onUnmounted(() => {
    window.removeEventListener('beforeunload', releaseBestEffort)
    release()
  })

  return { lockId, acquire, release }
}
