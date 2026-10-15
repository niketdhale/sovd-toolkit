<script setup lang="ts">
import { ref, computed } from 'vue'
import Icon from '../components/Icon.vue'
import type { EntityInfo } from '../api/sovdClient'

// Screen 1. Zero knowledge of any specific ECU: renders exactly whatever
// GET /v1/entities returns, nothing hardcoded.
const props = defineProps<{ entities: EntityInfo[] }>()
const emit = defineEmits<{ select: [path: string] }>()

const query = ref('')
const tree = ref(false)
const collapsed = ref(new Set<string>())

const filtered = computed(() => {
  const q = query.value.trim().toLowerCase()
  return props.entities.filter((e) => e.path.toLowerCase().includes(q))
})

// Rows: a real entity or a synthetic group node for a shared path prefix.
const rows = computed(() => {
  if (!tree.value) return filtered.value.map((e) => ({ key: e.path, label: e.path, depth: 0, group: false, entity: e }))
  const nodes = new Map<string, { entity?: EntityInfo; group: boolean }>()
  for (const e of filtered.value) {
    const segs = e.path.split('/')
    for (let i = 1; i <= segs.length; i++) {
      const k = segs.slice(0, i).join('/')
      const n = nodes.get(k) ?? { group: false }
      if (i < segs.length) n.group = true
      else n.entity = e
      nodes.set(k, n)
    }
  }
  return [...nodes.keys()]
    .sort()
    .filter((k) => {
      const segs = k.split('/')
      return !segs.slice(0, -1).some((_, i) => collapsed.value.has(segs.slice(0, i + 1).join('/')))
    })
    .map((k) => ({ key: k, label: k.split('/').pop()!, depth: k.split('/').length - 1, group: nodes.get(k)!.group, entity: nodes.get(k)!.entity }))
})

function toggle(k: string) {
  const s = new Set(collapsed.value)
  if (!s.delete(k)) s.add(k)
  collapsed.value = s
}
</script>

<template>
  <div>
    <div class="mb-5">
      <h2 class="text-base font-semibold text-gray-900 dark:text-white">Entities</h2>
      <p class="mt-1 text-sm text-gray-500 dark:text-gray-400">Everything this server reports via GET /v1/entities.</p>
    </div>

    <div class="mb-3 flex flex-wrap items-center gap-2">
      <label for="entity-search" class="sr-only">Filter by path</label>
      <input id="entity-search" v-model="query" type="search" class="field w-full max-w-xs" placeholder="Filter by path" />
      <button type="button" class="btn btn-secondary" :aria-pressed="tree" @click="tree = !tree">
        <Icon :name="tree ? 'list' : 'tree'" class="size-4" />{{ tree ? 'Flat list' : 'Tree' }}
      </button>
    </div>

    <div class="card overflow-x-auto">
      <table class="min-w-full divide-y divide-gray-200 dark:divide-white/10">
        <thead class="bg-gray-50 dark:bg-white/5">
          <tr>
            <th class="py-3.5 pl-4 pr-3 text-left text-sm font-semibold text-gray-900 sm:pl-6 dark:text-white">Path</th>
            <th class="hidden px-3 py-3.5 text-left text-sm font-semibold text-gray-900 sm:table-cell dark:text-white">Type</th>
            <th class="px-3 py-3.5 text-left text-sm font-semibold text-gray-900 dark:text-white">Backend</th>
            <th class="py-3.5 pl-3 pr-4 sm:pr-6"></th>
          </tr>
        </thead>
        <tbody class="divide-y divide-gray-200 dark:divide-white/10">
          <tr v-for="r in rows" :key="r.key" class="hover:bg-gray-50 dark:hover:bg-white/5">
            <td class="py-4 pl-4 pr-3 font-mono text-sm text-gray-900 sm:pl-6 dark:text-gray-100">
              <div class="flex items-center gap-1" :style="{ paddingLeft: r.depth * 1.25 + 'rem' }">
                <button v-if="r.group" type="button" class="rounded p-0.5 text-gray-500 hover:bg-gray-100 dark:text-gray-400 dark:hover:bg-white/10" :aria-expanded="!collapsed.has(r.key)" @click="toggle(r.key)">
                  <Icon name="chevron" class="size-4 transition-transform" :class="collapsed.has(r.key) ? '' : 'rotate-90'" /><span class="sr-only">Toggle {{ r.key }}</span>
                </button>
                <span v-else-if="tree" class="inline-block size-5"></span>
                <span :class="r.entity ? '' : 'text-gray-500 dark:text-gray-400'">{{ r.label }}</span>
              </div>
            </td>
            <td class="hidden px-3 py-4 text-sm text-gray-500 sm:table-cell dark:text-gray-400">{{ r.entity?.type }}</td>
            <td class="px-3 py-4 text-sm">
              <span v-if="r.entity" :class="r.entity.has_backend ? 'badge badge-green' : 'badge badge-gray'">{{ r.entity.has_backend ? 'yes' : 'no' }}</span>
            </td>
            <td class="py-4 pl-3 pr-4 text-right text-sm sm:pr-6">
              <button v-if="r.entity?.has_backend" class="font-medium text-indigo-600 hover:text-indigo-500 dark:text-indigo-400" @click="emit('select', r.entity.path)">
                Select<span class="sr-only">, {{ r.key }}</span>
              </button>
            </td>
          </tr>
        </tbody>
      </table>
      <div v-if="rows.length === 0" class="px-6 py-12 text-center text-sm text-gray-500 dark:text-gray-400">
        <Icon name="server" class="mx-auto size-8 text-gray-400 dark:text-gray-500" />
        <p class="mt-2">{{ entities.length ? 'No entities match.' : 'This server reports no entities yet.' }}</p>
      </div>
    </div>
  </div>
</template>
