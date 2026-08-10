import { useUiStore } from '@/stores/ui';
import type { ToastType } from '@/stores/ui';

export function useToast() {
  return useUiStore((s) => s.addToast) as (msg: string, type?: ToastType, ms?: number) => void;
}
