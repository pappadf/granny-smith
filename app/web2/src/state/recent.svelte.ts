// Reactive state for the Welcome page's Recent list (lib/recentMachines.ts).
// Loaded from and mirrored to localStorage by state/persist.svelte.ts.

import type { MachineConfig } from '@/bus/types';
import { forgetRecent, pushRecent, type RecentMachine } from '@/lib/recentMachines';

interface RecentState {
  entries: RecentMachine[];
}

export const recent: RecentState = $state({ entries: [] });

// Put a just-started machine on top of the list.
export function addRecentMachine(config: MachineConfig, label: string): void {
  // A snapshot: the list must not alias a caller's (possibly reactive) config.
  const entry = { config: $state.snapshot(config) as MachineConfig, label, lastUsed: Date.now() };
  recent.entries = pushRecent(recent.entries, entry);
}

// Drop one machine (the row's ×).
export function forgetRecentMachine(key: string): void {
  recent.entries = forgetRecent(recent.entries, key);
}
