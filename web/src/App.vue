<script setup lang="ts">
import { ref, computed } from 'vue'
import { SovdClient, type EntityInfo } from './api/sovdClient'
import EntityBrowser from './views/EntityBrowser.vue'
import FaultViewer from './views/FaultViewer.vue'
import DataTable from './views/DataTable.vue'
import LiveChart from './views/LiveChart.vue'

// D1 (settled 2026-08-20): points at a domain server directly, not the
// gateway -- streaming through a Phase 4 proxy is a deliberate 501, so
// screen 4 can't run against the gateway tier.
const baseUrlInput = ref(import.meta.env.VITE_SOVD_BASE_URL ?? 'http://localhost:20003')
const connectError = ref<string | null>(null)
const connecting = ref(false)
const client = ref<SovdClient | null>(null)
const serverId = ref('')
const role = ref('')

const entities = ref<EntityInfo[]>([])
const selectedPath = ref<string | null>(null)

const tabs = [
  { id: 'entities', label: 'Entities' },
  { id: 'faults', label: 'Faults' },
  { id: 'data', label: 'Data' },
  { id: 'live', label: 'Live' },
] as const
const tab = ref<(typeof tabs)[number]['id']>('entities')

const selectedEntity = computed(() => entities.value.find((e) => e.path === selectedPath.value) ?? null)

async function connect() {
  connectError.value = null
  connecting.value = true
  const url = baseUrlInput.value.replace(/\/+$/, '')
  const c = new SovdClient(url)
  try {
    const list = await c.listEntities()
    // Root info is discovery-driven too -- shown as-is, never assumed.
    const rootRes = await fetch(`${url}/`)
    const root = await rootRes.json()
    serverId.value = root.server_id
    role.value = root.role
    entities.value = list
    client.value = c
  } catch (e) {
    connectError.value = e instanceof Error ? e.message : String(e)
  } finally {
    connecting.value = false
  }
}

function selectEntity(path: string) {
  selectedPath.value = path
  tab.value = 'data'
}
</script>

<template>
  <div class="min-h-screen bg-gray-50 dark:bg-gray-950">
    <header class="border-b border-gray-200 bg-white dark:border-white/10 dark:bg-gray-900">
      <div class="mx-auto max-w-6xl px-4 py-4 sm:px-6 lg:px-8">
        <div class="flex flex-wrap items-center gap-x-6 gap-y-3">
          <div class="flex items-center gap-2">
            <span class="flex size-8 items-center justify-center rounded-lg bg-indigo-600 text-sm font-bold text-white">S</span>
            <h1 class="text-base font-semibold text-gray-900 dark:text-white">SOVD Toolkit</h1>
          </div>

          <form class="flex flex-1 items-center gap-2" @submit.prevent="connect">
            <input
              v-model="baseUrlInput"
              type="text"
              class="field w-full max-w-xs"
              placeholder="http://localhost:20003"
            />
            <button type="submit" class="btn btn-primary" :disabled="connecting">
              {{ connecting ? 'Connecting…' : client ? 'Reconnect' : 'Connect' }}
            </button>
          </form>

          <div v-if="client" class="flex items-center gap-2 text-sm">
            <span class="badge badge-green">
              <span class="mr-1 size-1.5 rounded-full bg-green-500"></span>
              {{ serverId }}
            </span>
            <span class="badge badge-gray capitalize">{{ role }}</span>
          </div>
        </div>
        <p v-if="connectError" class="mt-2 text-sm text-red-600 dark:text-red-400">{{ connectError }}</p>
      </div>

      <nav v-if="client" class="mx-auto max-w-6xl px-4 sm:px-6 lg:px-8">
        <div class="flex items-center justify-between border-t border-gray-100 dark:border-white/5">
          <div class="-mb-px flex gap-6">
            <button
              v-for="t in tabs"
              :key="t.id"
              class="border-b-2 px-1 py-3 text-sm font-medium transition-colors"
              :class="
                tab === t.id
                  ? 'border-indigo-500 text-indigo-600 dark:text-indigo-400'
                  : 'border-transparent text-gray-500 hover:border-gray-300 hover:text-gray-700 dark:text-gray-400 dark:hover:text-gray-200'
              "
              @click="tab = t.id"
            >
              {{ t.label }}
            </button>
          </div>
          <p v-if="selectedEntity" class="hidden text-sm text-gray-500 sm:block dark:text-gray-400">
            selected
            <code class="ml-1 rounded bg-gray-100 px-1.5 py-0.5 font-mono text-xs text-gray-700 dark:bg-white/10 dark:text-gray-300">{{
              selectedEntity.path
            }}</code>
          </p>
        </div>
      </nav>
    </header>

    <main v-if="client" class="mx-auto max-w-6xl px-4 py-8 sm:px-6 lg:px-8">
      <EntityBrowser v-if="tab === 'entities'" :entities="entities" @select="selectEntity" />
      <FaultViewer v-else-if="tab === 'faults'" :client="client" :path="selectedPath" />
      <DataTable v-else-if="tab === 'data'" :client="client" :path="selectedPath" />
      <LiveChart v-else-if="tab === 'live'" :client="client" :path="selectedPath" />
    </main>

    <main v-else class="mx-auto flex max-w-6xl flex-col items-center px-4 py-24 text-center sm:px-6 lg:px-8">
      <span class="flex size-12 items-center justify-center rounded-full bg-indigo-50 text-2xl dark:bg-indigo-500/10">🔌</span>
      <h2 class="mt-4 text-base font-semibold text-gray-900 dark:text-white">Not connected</h2>
      <p class="mt-1 max-w-sm text-sm text-gray-500 dark:text-gray-400">
        Enter a domain server's base URL above and click Connect to browse its entities, faults, and live data.
      </p>
    </main>
  </div>
</template>
