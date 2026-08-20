<script setup lang="ts">
import type { EntityInfo } from '../api/sovdClient'

// Screen 1. Zero knowledge of any specific ECU: renders exactly whatever
// GET /v1/entities returns, nothing hardcoded.
defineProps<{ entities: EntityInfo[] }>()
const emit = defineEmits<{ select: [path: string] }>()
</script>

<template>
  <div>
    <div class="mb-5">
      <h2 class="text-base font-semibold text-gray-900 dark:text-white">Entities</h2>
      <p class="mt-1 text-sm text-gray-500 dark:text-gray-400">Everything this server reports via GET /v1/entities.</p>
    </div>

    <div class="card overflow-hidden">
      <table class="min-w-full divide-y divide-gray-200 dark:divide-white/10">
        <thead class="bg-gray-50 dark:bg-white/5">
          <tr>
            <th class="py-3.5 pl-4 pr-3 text-left text-sm font-semibold text-gray-900 sm:pl-6 dark:text-white">Path</th>
            <th class="px-3 py-3.5 text-left text-sm font-semibold text-gray-900 dark:text-white">Type</th>
            <th class="px-3 py-3.5 text-left text-sm font-semibold text-gray-900 dark:text-white">Backend</th>
            <th class="py-3.5 pl-3 pr-4 sm:pr-6"></th>
          </tr>
        </thead>
        <tbody class="divide-y divide-gray-200 dark:divide-white/10">
          <tr v-for="e in entities" :key="e.path" class="hover:bg-gray-50 dark:hover:bg-white/5">
            <td class="py-4 pl-4 pr-3 font-mono text-sm text-gray-900 sm:pl-6 dark:text-gray-100">{{ e.path }}</td>
            <td class="px-3 py-4 text-sm text-gray-500 dark:text-gray-400">{{ e.type }}</td>
            <td class="px-3 py-4 text-sm">
              <span :class="e.has_backend ? 'badge badge-green' : 'badge badge-gray'">{{ e.has_backend ? 'yes' : 'no' }}</span>
            </td>
            <td class="py-4 pl-3 pr-4 text-right text-sm sm:pr-6">
              <button v-if="e.has_backend" class="font-medium text-indigo-600 hover:text-indigo-500 dark:text-indigo-400" @click="emit('select', e.path)">
                Select<span class="sr-only">, {{ e.path }}</span>
              </button>
            </td>
          </tr>
        </tbody>
      </table>
      <div v-if="entities.length === 0" class="px-6 py-12 text-center text-sm text-gray-500 dark:text-gray-400">No entities.</div>
    </div>
  </div>
</template>
