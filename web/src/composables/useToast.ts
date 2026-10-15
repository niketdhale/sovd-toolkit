import { ref } from 'vue'

export type Toast = { id: number; kind: 'success' | 'error'; text: string }

// Module-level state: one container in App.vue renders it, any view can push.
export const toasts = ref<Toast[]>([])
let nextId = 1

export function dismissToast(id: number) {
  toasts.value = toasts.value.filter((t) => t.id !== id)
}

export function useToast() {
  function push(kind: Toast['kind'], text: string, ms = 4000) {
    const id = nextId++
    toasts.value.push({ id, kind, text })
    setTimeout(() => dismissToast(id), ms)
  }
  return { success: (t: string) => push('success', t), error: (t: string) => push('error', t) }
}
