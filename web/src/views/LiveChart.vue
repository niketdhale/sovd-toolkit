<script setup lang="ts">
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
const points = ref<number[]>([])
const logLines = ref<string[]>([])
const error = ref<string | null>(null)
const streaming = ref(false)

let es: EventSource | null = null

const selectedItem = computed(() => docs.value?.data.find((d) => d.id === selectedId.value) ?? null)

function closeStream() {
  if (es) {
    es.close()
    es = null
  }
  streaming.value = false
}

async function loadDocs() {
  docs.value = null
  selectedId.value = ''
  closeStream()
  if (!props.path) return
  error.value = null
  try {
    docs.value = await props.client.getDocs(props.path)
  } catch (e) {
    error.value = e instanceof Error ? e.message : String(e)
  }
}

async function startStream() {
  closeStream()
  points.value = []
  logLines.value = []
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
    return
  }
  es = new EventSource(url)
  streaming.value = true
  es.onmessage = (ev) => {
    try {
      const data = JSON.parse(ev.data)
      if (data.error) {
        logLines.value.unshift(`error: ${data.error} — ${data.message ?? ''}`)
      } else if (selectedItem.value?.type === 'float') {
        points.value.push(Number(data.value))
        if (points.value.length > 50) points.value.shift()
      } else {
        logLines.value.unshift(`${new Date().toLocaleTimeString()}  ${JSON.stringify(data.value)}`)
        if (logLines.value.length > 50) logLines.value.pop()
      }
    } catch {
      // one malformed frame shouldn't kill the subscription
    }
  }
  es.onerror = () => {
    error.value = 'stream connection interrupted (browser will auto-retry)'
  }
}

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
    <span class="flex size-10 items-center justify-center rounded-full bg-gray-100 text-lg dark:bg-white/10">⚠</span>
    <p class="mt-3 text-sm text-gray-500 dark:text-gray-400">Select an entity from the Entities tab first.</p>
  </div>
  <div v-else>
    <div class="mb-5">
      <h2 class="text-base font-semibold text-gray-900 dark:text-white">Live</h2>
      <p class="mt-1 font-mono text-sm text-gray-500 dark:text-gray-400">{{ path }}</p>
    </div>

    <div class="card p-5">
      <div class="flex flex-wrap items-center gap-3">
        <select v-model="selectedId" class="field" @change="closeStream">
          <option value="" disabled>select a data id…</option>
          <option v-for="item in docs?.data ?? []" :key="item.id" :value="item.id">{{ item.id }} ({{ item.type }})</option>
        </select>
        <label class="flex items-center gap-1.5 text-sm text-gray-500 dark:text-gray-400">
          interval
          <input v-model.number="intervalMs" type="number" min="100" step="100" class="field w-20" />
          ms
        </label>
        <button class="btn" :class="streaming ? 'btn-danger' : 'btn-primary'" :disabled="!selectedId" @click="streaming ? closeStream() : startStream()">
          <span v-if="streaming" class="size-1.5 animate-pulse rounded-full bg-white"></span>
          {{ streaming ? 'Stop' : 'Start streaming' }}
        </button>
      </div>
      <p v-if="error" class="mt-3 text-sm text-amber-600 dark:text-amber-400">{{ error }}</p>

      <div v-if="selectedItem?.type === 'float'" class="mt-5 border-t border-gray-100 pt-5 dark:border-white/10">
        <svg viewBox="0 0 600 160" class="w-full text-indigo-600 dark:text-indigo-400" preserveAspectRatio="none">
          <polyline :points="svgPoints" fill="none" stroke="currentColor" stroke-width="2" stroke-linejoin="round" />
        </svg>
        <p class="mt-2 text-xs text-gray-500 dark:text-gray-400">
          last {{ points.length }} value(s)<template v-if="points.length">, most recent
          <span class="font-semibold text-gray-900 dark:text-gray-100">{{ points[points.length - 1] }} {{ selectedItem?.unit ?? '' }}</span></template>
        </p>
      </div>

      <ul
        v-else-if="selectedId"
        class="mt-5 max-h-64 space-y-1 overflow-y-auto border-t border-gray-100 pt-5 font-mono text-xs text-gray-600 dark:border-white/10 dark:text-gray-400"
      >
        <li v-for="(line, i) in logLines" :key="i">{{ line }}</li>
      </ul>
    </div>
  </div>
</template>
