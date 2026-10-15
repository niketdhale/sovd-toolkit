<script setup lang="ts">
import Icon from '../components/Icon.vue'
import { ref, watch, computed, onUnmounted } from 'vue'
import type { SovdClient, DocsResult, DataItemDoc, DataValue } from '../api/sovdClient'
import { useLock } from '../composables/useLock'
import { useToast } from '../composables/useToast'

// Screen 3. Every widget is chosen purely from item.type/item.access/
// item.values coming back from GET .../docs -- no entity- or DID-specific
// code anywhere here. B1 (catalog encode()) is what makes the write side
// possible at all: a typed value goes straight over the wire now, not hex.
const props = defineProps<{ client: SovdClient; path: string | null }>()

const docs = ref<DocsResult | null>(null)
const values = ref<Record<string, DataValue>>({})
const drafts = ref<Record<string, string>>({})
const error = ref<string | null>(null)
const writingId = ref<string | null>(null)
const loading = ref(false)

const showJson = ref(false)
const refreshedAt = ref<Date | null>(null)
const autoSecs = ref(0)
const copiedId = ref<string | null>(null)
let timer: ReturnType<typeof setInterval> | undefined

async function copyValue(id: string) {
  try {
    await navigator.clipboard.writeText(String(values.value[id]?.value ?? ''))
    copiedId.value = id
    setTimeout(() => { if (copiedId.value === id) copiedId.value = null }, 1500)
  } catch (e) {
    error.value = e instanceof Error ? e.message : String(e)
  }
}

function setTimer() {
  clearInterval(timer)
  timer = autoSecs.value && props.path ? setInterval(() => { if (!loading.value && !editMode.value) load(true) }, autoSecs.value * 1000) : undefined
}
watch(autoSecs, setTimer)
onUnmounted(() => clearInterval(timer))

const toast = useToast()
const lock = useLock(props.client, () => props.path!)
const editMode = computed(() => lock.lockId.value !== null)

function accessBadge(access: string) {
  if (access === 'read') return 'badge badge-gray'
  if (access === 'write') return 'badge badge-indigo'
  return 'badge badge-green'
}

async function load(silent = false) {
  if (!silent) {
    docs.value = null
    values.value = {}
  }
  if (!props.path) return
  error.value = null
  loading.value = true
  try {
    const d = await props.client.getDocs(props.path)
    docs.value = d
    if (d.data.length === 0) return
    const batch = await props.client.getDataBatch(
      props.path,
      d.data.map((i) => i.id),
    )
    const valueMap: Record<string, DataValue> = {}
    const draftMap: Record<string, string> = {}
    for (const v of batch) {
      valueMap[v.id] = v
      draftMap[v.id] = v.value !== undefined ? String(v.value) : ''
    }
    values.value = valueMap
    drafts.value = draftMap
    refreshedAt.value = new Date()
  } catch (e) {
    error.value = e instanceof Error ? e.message : String(e)
  } finally {
    loading.value = false
  }
}

async function toggleEdit() {
  error.value = null
  try {
    if (editMode.value) await lock.release()
    else await lock.acquire()
    if (editMode.value) toast.success('Lock acquired')
    else toast.success('Lock released')
  } catch (e) {
    error.value = e instanceof Error ? e.message : String(e)
    toast.error(`Lock failed: ${error.value}`)
  }
}

async function writeItem(item: DataItemDoc) {
  if (!props.path || !lock.lockId.value) return
  writingId.value = item.id
  error.value = null
  try {
    const raw = drafts.value[item.id] ?? ''
    const value: unknown = item.type === 'float' ? Number(raw) : raw
    await props.client.putData(props.path, item.id, value, lock.lockId.value)
    await load()
    toast.success(`Wrote ${item.id}`)
  } catch (e) {
    error.value = e instanceof Error ? e.message : String(e)
    toast.error(`Write failed: ${error.value}`)
  } finally {
    writingId.value = null
  }
}

watch(
  () => props.path,
  async (_next, prev) => {
    if (prev && editMode.value) await lock.release()
    setTimer()
    await load()
  },
  { immediate: true },
)
defineExpose({ refresh: () => load() })
</script>

<template>
  <div v-if="!path" class="card flex flex-col items-center px-6 py-16 text-center">
    <span class="flex size-10 items-center justify-center rounded-full bg-gray-100 text-gray-500 dark:bg-white/10 dark:text-gray-400"><Icon name="warning" class="size-5" /></span>
    <p class="mt-3 text-sm text-gray-500 dark:text-gray-400">Select an entity from the Entities tab first.</p>
  </div>
  <div v-else>
    <div class="mb-5 flex flex-wrap items-end justify-between gap-3">
      <div>
        <h2 class="text-base font-semibold text-gray-900 dark:text-white">Data</h2>
        <p class="mt-1 font-mono text-sm text-gray-500 dark:text-gray-400">{{ path }}</p>
      </div>
      <div class="flex flex-wrap items-center gap-2">
        <span v-if="refreshedAt" class="text-xs text-gray-500 dark:text-gray-400">refreshed {{ refreshedAt.toLocaleTimeString() }}</span>
        <label for="auto-refresh" class="sr-only">Auto-refresh</label>
        <select id="auto-refresh" v-model.number="autoSecs" class="field">
          <option :value="0">Auto-refresh: off</option>
          <option :value="5">Every 5s</option>
          <option :value="10">Every 10s</option>
        </select>
        <button type="button" class="btn btn-secondary" :aria-pressed="showJson" @click="showJson = !showJson">{{ showJson ? 'Table' : 'Raw JSON' }}</button>
        <span v-if="editMode" class="text-xs text-gray-500 dark:text-gray-400">lock renews every ~5s (10s TTL)</span>
        <button class="btn" :class="editMode ? 'bg-amber-100 text-amber-800 hover:bg-amber-200 dark:bg-amber-500/10 dark:text-amber-400' : 'btn-primary'" @click="toggleEdit">
          {{ editMode ? 'Stop editing' : 'Enable editing' }}
        </button>
      </div>
    </div>

    <div v-if="error" role="alert" class="alert-error">
      <span>{{ error }}</span>
      <button type="button" class="btn btn-secondary shrink-0" @click="load()"><Icon name="retry" class="size-4" />Retry</button>
    </div>

    <pre v-if="showJson" class="card overflow-x-auto p-4 font-mono text-xs text-gray-900 dark:text-gray-100">{{ JSON.stringify(Object.values(values), null, 2) }}</pre>
    <div v-else class="card overflow-x-auto">
      <table class="min-w-full divide-y divide-gray-200 dark:divide-white/10">
        <thead class="bg-gray-50 dark:bg-white/5">
          <tr>
            <th class="py-3.5 pl-4 pr-3 text-left text-sm font-semibold text-gray-900 sm:pl-6 dark:text-white">Id</th>
            <th class="px-3 py-3.5 text-left text-sm font-semibold text-gray-900 dark:text-white">DID</th>
            <th class="px-3 py-3.5 text-left text-sm font-semibold text-gray-900 dark:text-white">Type</th>
            <th class="px-3 py-3.5 text-left text-sm font-semibold text-gray-900 dark:text-white">Access</th>
            <th class="px-3 py-3.5 text-left text-sm font-semibold text-gray-900 dark:text-white">Value</th>
            <th class="py-3.5 pl-3 pr-4 sm:pr-6"></th>
          </tr>
        </thead>
        <tbody class="divide-y divide-gray-200 dark:divide-white/10">
          <tr v-for="n in loading && !docs ? 4 : 0" :key="'s' + n">
            <td v-for="c in 5" :key="c" class="px-3 py-4 first:pl-4 sm:first:pl-6"><div class="skeleton h-4 w-20"></div></td>
            <td></td>
          </tr>
          <tr v-for="item in docs?.data ?? []" :key="item.id" class="hover:bg-gray-50 dark:hover:bg-white/5">
            <td class="py-4 pl-4 pr-3 font-mono text-sm text-gray-900 sm:pl-6 dark:text-gray-100">{{ item.id }}</td>
            <td class="px-3 py-4 font-mono text-sm text-gray-400 dark:text-gray-500">{{ item.did }}</td>
            <td class="px-3 py-4 text-sm text-gray-500 dark:text-gray-400">{{ item.type }}</td>
            <td class="px-3 py-4 text-sm"><span :class="accessBadge(item.access)">{{ item.access }}</span></td>
            <td class="px-3 py-4 text-sm">
              <span v-if="values[item.id]?.error" class="text-red-600 dark:text-red-400">{{ values[item.id].error }}</span>
              <template v-else-if="editMode && item.access !== 'read'">
                <div class="flex items-center gap-1.5">
                  <select v-if="item.type === 'enum'" v-model="drafts[item.id]" class="field">
                    <option v-for="[raw, label] in Object.entries(item.values ?? {})" :key="raw" :value="label">
                      {{ label }}
                    </option>
                  </select>
                  <input v-else-if="item.type === 'float'" v-model="drafts[item.id]" type="number" step="any" class="field w-28" />
                  <input v-else v-model="drafts[item.id]" type="text" :maxlength="item.length || undefined" class="field w-40 font-mono" />
                  <span v-if="item.unit" class="text-xs text-gray-400 dark:text-gray-500">{{ item.unit }}</span>
                </div>
              </template>
              <template v-else>
                <span class="font-medium text-gray-900 dark:text-gray-100">{{ values[item.id]?.value }}</span>
                <span v-if="item.unit" class="ml-1 text-xs text-gray-400 dark:text-gray-500">{{ item.unit }}</span>
              </template>
            </td>
            <td class="py-4 pl-3 pr-4 text-right sm:pr-6">
              <button
                v-if="!editMode || item.access === 'read'"
                type="button"
                class="btn btn-secondary"
                :disabled="values[item.id]?.value === undefined"
                @click="copyValue(item.id)"
              >
                <Icon :name="copiedId === item.id ? 'check' : 'copy'" class="size-4" />{{ copiedId === item.id ? 'Copied' : 'Copy value' }}
              </button>
              <button
                v-if="editMode && item.access !== 'read'"
                class="rounded-md bg-indigo-600 px-2.5 py-1 text-xs font-semibold text-white shadow-xs hover:bg-indigo-500 focus-visible:outline focus-visible:outline-2 focus-visible:outline-offset-2 focus-visible:outline-indigo-600 disabled:cursor-not-allowed disabled:opacity-50"
                :disabled="writingId === item.id"
                @click="writeItem(item)"
              >
                {{ writingId === item.id ? 'Writing…' : 'Write' }}
              </button>
            </td>
          </tr>
        </tbody>
      </table>
      <div v-if="docs && docs.data.length === 0" class="px-6 py-12 text-center text-sm text-gray-500 dark:text-gray-400">
        <Icon name="inbox" class="mx-auto size-8 text-gray-400 dark:text-gray-500" />
        <p class="mt-2">No named data items in this entity's catalog.</p>
      </div>
    </div>
  </div>
</template>
