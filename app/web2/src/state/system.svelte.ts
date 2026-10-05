// The SYSTEM tab's view state, kept while the tab is closed: which levels
// are open (by path, so they survive a reload of the tree), whether the
// advanced tier is shown, and a node another surface asked to reveal.
import { setActiveTab } from './layout.svelte';
import { pathPrefixes } from '@/lib/objectPath';

export const systemView: {
  expanded: Record<string, boolean>;
  showAdvanced: boolean;
  // A path to select once the tab shows it (then cleared).
  reveal: string;
} = $state({
  expanded: {},
  showAdvanced: false,
  reveal: '',
});

// Open the SYSTEM tab at `path` (a console object link, Ctrl/Cmd-click):
// the levels above it open, and the row is selected when it appears.
export function revealInSystem(path: string): void {
  for (const p of pathPrefixes(path)) if (p !== path) systemView.expanded[p] = true;
  systemView.reveal = path;
  setActiveTab('machine');
}
