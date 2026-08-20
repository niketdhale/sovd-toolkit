<script setup lang="ts">
import { ref, watch } from 'vue'
import type { SovdClient, FaultInfo } from '../api/sovdClient'

const props = defineProps<{ client: SovdClient; path: string | null }>()

const faults = ref<FaultInfo[]>([])
const statusFilter = ref('')
const error = ref<string | null>(null)
const clearing = ref(false)
const loading = ref(false)

function badgeFor(status: string) {
  if (status === 'confirmed') return 'badge badge-red'
  if (status === 'pending') return 'badge badge-amber'
  return 'badge badge-gray'
}

async function load() {
  if (!props.path) return
  loading.value = true
  error.value = null
  try {
    faults.value = await props.client.getFaults(props.path, statusFilter.value)
  } catch (e) {
    error.value = e instanceof Error ? e.message : String(e)
  } finally {
    loading.value = false
  }
}

// A one-shot lock -- acquire, clear, release, all within one click. No
// heartbeat needed (the whole thing resolves in a single round trip, not
// held open across user think-time), matching the CLI's own
// `faults-clear` command (LockGuard with heartbeat=false for the same
// reason). useLock's heartbeat+beforeunload machinery is for DataTable's
// held-open edit sessions, not this.
async function clearFaults() {
  if (!props.path) return
  clearing.value = true
  error.value = null
  let lockId: string | null = null
  try {
    lockId = await props.client.acquireLock(props.path, 10)
    await props.client.clearFaults(props.path, lockId)
    await load()
  } catch (e) {
    error.value = e instanceof Error ? e.message : String(e)
  } finally {
    if (lockId) await props.client.releaseLock(props.path, lockId).catch(() => {})
    clearing.value = false
  }
}

watch(() => [props.path, statusFilter.value], load, { immediate: true })
</script>

<template>
  <div v-if="!path" class="card flex flex-col items-center px-6 py-16 text-center">
    <span class="flex size-10 items-center justify-center rounded-full bg-gray-100 text-lg dark:bg-white/10">⚠</span>
    <p class="mt-3 text-sm text-gray-500 dark:text-gray-400">Select an entity from the Entities tab first.</p>
  </div>
  <div v-else>
    <div class="mb-5 flex flex-wrap items-end justify-between gap-3">
      <div>
        <h2 class="text-base font-semibold text-gray-900 dark:text-white">Faults</h2>
        <p class="mt-1 font-mono text-sm text-gray-500 dark:text-gray-400">{{ path }}</p>
      </div>
      <div class="flex items-center gap-2">
        <select v-model="statusFilter" class="field">
          <option value="">all statuses</option>
          <option value="confirmed">confirmed</option>
          <option value="pending">pending</option>
          <option value="testFailed">testFailed</option>
        </select>
        <button class="btn btn-danger" :disabled="clearing || faults.length === 0" @click="clearFaults">
          {{ clearing ? 'Clearing…' : 'Clear faults' }}
        </button>
      </div>
    </div>

    <p v-if="error" class="mb-3 text-sm text-red-600 dark:text-red-400">{{ error }}</p>

    <div class="card overflow-hidden">
      <table class="min-w-full divide-y divide-gray-200 dark:divide-white/10">
        <thead class="bg-gray-50 dark:bg-white/5">
          <tr>
            <th class="py-3.5 pl-4 pr-3 text-left text-sm font-semibold text-gray-900 sm:pl-6 dark:text-white">Code</th>
            <th class="px-3 py-3.5 text-left text-sm font-semibold text-gray-900 dark:text-white">Status</th>
          </tr>
        </thead>
        <tbody class="divide-y divide-gray-200 dark:divide-white/10">
          <tr v-for="f in faults" :key="f.code" class="hover:bg-gray-50 dark:hover:bg-white/5">
            <td class="py-4 pl-4 pr-3 font-mono text-sm text-gray-900 sm:pl-6 dark:text-gray-100">{{ f.code }}</td>
            <td class="px-3 py-4 text-sm"><span :class="badgeFor(f.status)">{{ f.status }}</span></td>
          </tr>
        </tbody>
      </table>
      <div v-if="!loading && faults.length === 0" class="px-6 py-12 text-center text-sm text-gray-500 dark:text-gray-400">
        No faults{{ statusFilter ? ` with status "${statusFilter}"` : '' }}.
      </div>
    </div>
  </div>
</template>
