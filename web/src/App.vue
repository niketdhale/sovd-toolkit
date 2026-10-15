<script setup lang="ts">
import Icon from './components/Icon.vue'
import { ref, computed, watchEffect } from 'vue'
import { SovdClient, SovdError, type EntityInfo } from './api/sovdClient'
import EntityBrowser from './views/EntityBrowser.vue'
import FaultViewer from './views/FaultViewer.vue'
import DataTable from './views/DataTable.vue'
import LiveChart from './views/LiveChart.vue'

// D1 (settled 2026-08-20): points at a domain server directly, not the
// gateway -- streaming through a Phase 4 proxy is a deliberate 501, so
// screen 4 can't run against the gateway tier.
let savedUrl: string | null = null
try { savedUrl = localStorage.getItem('baseUrl') } catch { /* ignore */ }
const baseUrlInput = ref(savedUrl ?? import.meta.env.VITE_SOVD_BASE_URL ?? 'http://localhost:20003')
// docs/reviews/round-1.md Task 1b: optional -- an unauthenticated server
// (this project's default demo config) never checks this at all, matching
// SovdClient.token's own "unset means no Authorization header" behavior.
const tokenInput = ref('')
const connectError = ref<string | null>(null)
const connecting = ref(false)
const editing = ref(false) // expands the connect form while connected
const client = ref<SovdClient | null>(null)
const serverId = ref('')
const role = ref('')

// Dark mode: stored choice, else OS preference. Storage may be unavailable.
let saved: string | null = null
try { saved = localStorage.getItem('theme') } catch { /* ignore */ }
const dark = ref(saved ? saved === 'dark' : window.matchMedia('(prefers-color-scheme: dark)').matches)
watchEffect(() => {
  document.documentElement.classList.toggle('dark', dark.value)
  try { localStorage.setItem('theme', dark.value ? 'dark' : 'light') } catch { /* ignore */ }
})

function disconnect() {
  client.value = null
  entities.value = []
  selectedPath.value = null
  tab.value = 'entities'
}

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
  c.token = tokenInput.value.trim() || null
  try {
    const list = await c.listEntities()
    // Root info is discovery-driven too -- shown as-is, never assumed.
    const rootRes = await fetch(`${url}/`)
    const root = await rootRes.json()
    serverId.value = root.server_id
    role.value = root.role
    entities.value = list
    client.value = c
    editing.value = false
    // Only the URL is remembered -- never the token.
    try { localStorage.setItem('baseUrl', url) } catch { /* ignore */ }
  } catch (e) {
    // docs/reviews/round-1.md Task 1b: 401 ("no/invalid token") and 403
    // ("valid token, wrong scope") are different user problems -- the
    // server already distinguishes them (routes.cpp's check_oauth2), so
    // this labels which one happened instead of showing a flat error.
    if (e instanceof SovdError && e.status === 401) {
      connectError.value = `Not authenticated — paste a valid bearer token above (${e.message}).`
    } else if (e instanceof SovdError && e.status === 403) {
      connectError.value = `Authenticated, but this token lacks a required scope (${e.message}).`
    } else {
      connectError.value = e instanceof Error ? e.message : String(e)
    }
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

          <form v-if="!client || editing" class="flex min-w-0 flex-1 flex-wrap items-center gap-2" @submit.prevent="connect">
            <label for="base-url" class="sr-only">Server base URL</label>
            <input
              id="base-url"
              v-model="baseUrlInput"
              type="text"
              class="field w-full max-w-xs"
              placeholder="http://localhost:20003"
            />
            <label for="token" class="sr-only">Bearer token</label>
            <input
              id="token"
              v-model="tokenInput"
              type="password"
              class="field w-full max-w-xs"
              placeholder="bearer token (only if OAuth2 is enabled)"
              autocomplete="off"
            />
            <button type="submit" class="btn btn-primary" :disabled="connecting">
              {{ connecting ? 'Connecting…' : client ? 'Reconnect' : 'Connect' }}
            </button>
          </form>
          <div v-else class="flex min-w-0 basis-full items-center gap-2 sm:basis-0 sm:flex-1">
            <span class="badge badge-gray min-w-0">
              <span class="truncate">connected to {{ client.baseUrl }}</span>
            </span>
            <button type="button" class="btn btn-secondary" aria-label="Edit connection" @click="editing = true">
              <Icon name="pencil" class="size-4" />Edit
            </button>
          </div>

          <div class="flex flex-wrap items-center gap-2 text-sm" role="status" aria-live="polite">
            <template v-if="client">
              <span class="badge badge-green">
                <span class="mr-1.5 size-1.5 rounded-full bg-green-500"></span>
                Connected · {{ serverId }}
              </span>
              <span class="badge badge-gray capitalize">{{ role }}</span>
              <button type="button" class="btn btn-secondary" @click="disconnect">Disconnect</button>
            </template>
            <span v-else-if="connecting" class="badge badge-amber">
              <span class="mr-1.5 size-1.5 animate-pulse rounded-full bg-amber-500"></span>Connecting…
            </span>
            <span v-else class="badge badge-gray">
              <span class="mr-1.5 size-1.5 rounded-full" :class="connectError ? 'bg-red-500' : 'bg-gray-400'"></span>
              {{ connectError ? 'Connection failed' : 'Disconnected' }}
            </span>
            <button
              type="button"
              class="btn btn-secondary"
              :aria-pressed="dark"
              aria-label="Toggle dark mode"
              @click="dark = !dark"
            >
              <Icon :name="dark ? 'sun' : 'moon'" class="size-4" />
              {{ dark ? 'Light' : 'Dark' }}
            </button>
          </div>
        </div>
        <p v-if="connectError" role="alert" class="mt-2 text-sm text-red-600 dark:text-red-400">{{ connectError }}</p>
      </div>

      <nav v-if="client" aria-label="Sections" class="mx-auto max-w-6xl px-4 sm:px-6 lg:px-8">
        <div class="flex items-center justify-between gap-4 border-t border-gray-100 dark:border-white/5">
          <div class="-mb-px flex gap-6 overflow-x-auto">
            <button
              v-for="t in tabs"
              :key="t.id"
              class="border-b-2 px-1 py-3 text-sm font-medium transition-colors"
              :class="
                tab === t.id
                  ? 'border-indigo-500 text-indigo-600 dark:text-indigo-400'
                  : 'border-transparent text-gray-500 hover:border-gray-300 hover:text-gray-700 dark:text-gray-400 dark:hover:text-gray-200'
              "
              :aria-current="tab === t.id ? 'page' : undefined"
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

    <main v-else role="status" class="mx-auto flex max-w-6xl flex-col items-center px-4 py-24 text-center sm:px-6 lg:px-8">
      <span class="flex size-12 items-center justify-center rounded-full bg-indigo-50 dark:bg-indigo-500/10"><Icon name="plug" class="size-6 text-indigo-600 dark:text-indigo-400" /></span>
      <h2 class="mt-4 text-base font-semibold text-gray-900 dark:text-white">Not connected</h2>
      <p class="mt-1 max-w-sm text-sm text-gray-500 dark:text-gray-400">
        Enter a domain server's base URL above and click Connect to browse its entities, faults, and live data.
      </p>
    </main>
  </div>
</template>
