<script setup lang="ts">
import Icon from '../components/Icon.vue'
import { ref, watch, onUnmounted, computed } from 'vue'
import type { SovdClient, DocsResult } from '../api/sovdClient'

// Screen 4 -- the clearest "why SOVD over UDS" demo (docs/DESIGN.md). Native
// EventSource, not a hand-rolled SSE client: it's the browser's own API for
// this, and B2 already verified CORS on the stream endpoint specifically
// for it. No charting dependency for one screen -- a hand-rolled SVG
// polyline is enough for "the last N points of one float value."
const props = defineProps<{ client: SovdClient; path: string | null }>()

const docs = ref<DocsResult | null>(null)
const selectedId = ref('')
const intervalMs = ref(1000)
const samples = ref<{ t: number; v: number }[]>([])
const windowSec = ref(60)
const logLines = ref<string[]>([])
const error = ref<string | null>(null)
const streaming = ref(false)
const loadingDocs = ref(false)
const streamState = ref<'stopped' | 'live' | 'paused' | 'reconnecting' | 'error'>('stopped')

let es: EventSource | null = null
let retryTimer: ReturnType<typeof setTimeout> | undefined
let backoffMs = 1000

const selectedItem = computed(() => docs.value?.data.find((d) => d.id === selectedId.value) ?? null)

function closeStream() {
  clearTimeout(retryTimer)
  if (es) {
    es.close()
    es = null
  }
  streaming.value = false
  streamState.value = 'stopped'
}

function pauseStream() {
  closeStream()
  streamState.value = 'paused'
}

async function loadDocs() {
  docs.value = null
  selectedId.value = ''
  closeStream()
  if (!props.path) return
  error.value = null
  loadingDocs.value = true
  try {
    docs.value = await props.client.getDocs(props.path)
  } catch (e) {
    error.value = e instanceof Error ? e.message : String(e)
    streamState.value = 'error'
  } finally {
    loadingDocs.value = false
  }
}

async function startStream() {
  samples.value = []
  logLines.value = []
  await openStream()
}

// Opens (or re-opens, on resume/reconnect) without clearing collected points.
async function openStream() {
  closeStream()
  error.value = null
  if (!props.path || !selectedId.value) return

  // streamUrl() now mints a stream ticket first when the client has a
  // token configured (Task 1c) -- a bearer-authenticated POST that can
  // itself 401/403, unlike before when this was a synchronous string build.
  let url: string
  try {
    url = await props.client.streamUrl(props.path, selectedId.value, intervalMs.value)
  } catch (e) {
    error.value = e instanceof Error ? e.message : String(e)
    streamState.value = 'error'
    return
  }
  es = new EventSource(url)
  streaming.value = true
  streamState.value = 'live'
  es.onopen = () => {
    backoffMs = 1000
    streamState.value = 'live'
    error.value = null
  }
  es.onmessage = (ev) => {
    try {
      const data = JSON.parse(ev.data)
      if (data.error) {
        logLines.value.unshift(`error: ${data.error} — ${data.message ?? ''}`)
      } else if (selectedItem.value?.type === 'float') {
        const now = Date.now()
        samples.value.push({ t: now, v: Number(data.value) })
        // keep at most the largest window (5m) in memory
        const keep = now - 300_000
        while (samples.value.length && samples.value[0].t < keep) samples.value.shift()
      } else {
        logLines.value.unshift(`${new Date().toLocaleTimeString()}  ${JSON.stringify(data.value)}`)
        if (logLines.value.length > 50) logLines.value.pop()
      }
    } catch {
      // one malformed frame shouldn't kill the subscription
    }
  }
  es.onerror = () => {
    // Close and reopen ourselves: a ticketed URL is single-use, and the
    // browser gives up for good on some errors. Exponential backoff, capped.
    es?.close()
    es = null
    streamState.value = 'reconnecting'
    error.value = `stream interrupted, retrying in ${backoffMs / 1000}s`
    retryTimer = setTimeout(openStream, backoffMs)
    backoffMs = Math.min(backoffMs * 2, 30000)
  }
}

const points = computed(() => {
  const from = Date.now() - windowSec.value * 1000
  // samples is reactive, so this recomputes on every new frame
  return samples.value.filter((s) => s.t >= from).map((s) => s.v)
})
const stats = computed(() =>
  points.value.length ? { min: Math.min(...points.value), max: Math.max(...points.value), latest: points.value[points.value.length - 1] } : null,
)

const svgPoints = computed(() => {
  if (points.value.length < 2) return ''
  const min = Math.min(...points.value)
  const max = Math.max(...points.value)
  const range = max - min || 1
  const w = 600
  const h = 160
  return points.value
    .map((v, i) => {
      const x = (i / (points.value.length - 1)) * w
      const y = h - ((v - min) / range) * h
      return `${x.toFixed(1)},${y.toFixed(1)}`
    })
    .join(' ')
})

watch(() => props.path, loadDocs, { immediate: true })
onUnmounted(closeStream)
</script>

<template>
  <div v-if="!path" class="card flex flex-col items-center px-6 py-16 text-center">
    <span class="flex size-10 items-center justify-center rounded-full bg-gray-100 text-gray-500 dark:bg-white/10 dark:text-gray-400"><Icon name="warning" class="size-5" /></span>
    <p class="mt-3 text-sm text-gray-500 dark:text-gray-400">Select an entity from the Entities tab first.</p>
  </div>
  <div v-else>
    <div class="mb-5">
      <h2 class="text-base font-semibold text-gray-900 dark:text-white">Live</h2>
      <p class="mt-1 font-mono text-sm text-gray-500 dark:text-gray-400">{{ path }}</p>
    </div>

    <div class="card p-5">
      <div class="flex flex-wrap items-center gap-3">
        <div v-if="loadingDocs" class="skeleton h-9 w-48"></div>
        <select v-else v-model="selectedId" class="field" @change="closeStream">
          <option value="" disabled>select a data id…</option>
          <option v-for="item in docs?.data ?? []" :key="item.id" :value="item.id">{{ item.id }} ({{ item.type }})</option>
        </select>
        <label class="flex items-center gap-1.5 text-sm text-gray-500 dark:text-gray-400">
          interval
          <input v-model.number="intervalMs" type="number" min="100" step="100" class="field w-20" />
          ms
        </label>
        <label class="flex items-center gap-1.5 text-sm text-gray-500 dark:text-gray-400">
          window
          <select v-model.number="windowSec" class="field" aria-label="Time window">
            <option :value="30">30s</option>
            <option :value="60">1m</option>
            <option :value="300">5m</option>
          </select>
        </label>
        <span
          class="badge"
          :class="{ 'badge-green': streamState === 'live', 'badge-gray': streamState === 'stopped' || streamState === 'paused', 'badge-amber': streamState === 'reconnecting', 'badge-red': streamState === 'error' }"
          role="status"
        >
          <span class="mr-1.5 size-1.5 rounded-full" :class="{ 'animate-pulse bg-green-500': streamState === 'live', 'bg-gray-400': streamState === 'stopped' || streamState === 'paused', 'animate-pulse bg-amber-500': streamState === 'reconnecting', 'bg-red-500': streamState === 'error' }"></span>
          {{ { live: 'Live', paused: 'Paused', reconnecting: 'Reconnecting…', error: 'Error', stopped: 'Stopped' }[streamState] }}
        </span>
        <button v-if="streaming || streamState === 'reconnecting' || streamState === 'paused'" class="btn btn-secondary" @click="streamState === 'paused' ? openStream() : pauseStream()">
          {{ streamState === 'paused' ? 'Resume' : 'Pause' }}
        </button>
        <button class="btn" :class="streaming ? 'btn-danger' : 'btn-primary'" :disabled="!selectedId" @click="streaming || streamState !== 'stopped' ? closeStream() : startStream()">
          <span v-if="streaming" class="size-1.5 animate-pulse rounded-full bg-white"></span>
          {{ streaming || streamState === 'reconnecting' || streamState === 'paused' ? 'Stop' : 'Start streaming' }}
        </button>
      </div>
      <div v-if="error" role="alert" class="alert-error mt-4 mb-0">
        <span>{{ error }}</span>
        <button type="button" class="btn btn-secondary shrink-0" @click="docs ? startStream() : loadDocs()"><Icon name="retry" class="size-4" />Retry</button>
      </div>
      <p v-if="!loadingDocs && !error && !selectedId" class="mt-5 border-t border-gray-100 pt-5 text-center text-sm text-gray-500 dark:border-white/10 dark:text-gray-400">
        <Icon name="chart" class="mr-1 inline size-4 align-text-bottom text-gray-400 dark:text-gray-500" />Pick a data id and start streaming to see live values here.
      </p>

      <div v-if="selectedItem?.type === 'float'" class="mt-5 border-t border-gray-100 pt-5 dark:border-white/10">
        <svg viewBox="0 0 600 160" class="w-full text-indigo-600 dark:text-indigo-400" preserveAspectRatio="none">
          <polyline :points="svgPoints" fill="none" stroke="currentColor" stroke-width="2" stroke-linejoin="round" />
        </svg>
        <p class="mt-2 text-xs text-gray-500 dark:text-gray-400">
          last {{ points.length }} value(s)<template v-if="stats">
            · min <span class="font-semibold text-gray-900 dark:text-gray-100">{{ stats.min.toFixed(2) }}</span>
            · max <span class="font-semibold text-gray-900 dark:text-gray-100">{{ stats.max.toFixed(2) }}</span>
            · latest <span class="font-semibold text-gray-900 dark:text-gray-100">{{ stats.latest }} {{ selectedItem?.unit ?? '' }}</span></template>
        </p>
      </div>

      <ul
        v-else-if="selectedId"
        class="mt-5 max-h-64 space-y-1 overflow-y-auto border-t border-gray-100 pt-5 font-mono text-xs text-gray-600 dark:border-white/10 dark:text-gray-400"
      >
        <li v-if="logLines.length === 0" class="font-sans text-gray-500">Waiting for values…</li>
        <li v-for="(line, i) in logLines" :key="i">{{ line }}</li>
      </ul>
    </div>
  </div>
</template>
