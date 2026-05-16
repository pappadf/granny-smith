// Pure helpers for machine config / model identification.

import type { MachineConfig } from '@/bus/types';

// SE/30 and II-series have a 68030 with an integrated MMU. Plus and SE have
// a 68000, no MMU. Prototype source: app.js `modelHasMmu`.
export function modelHasMmu(model: string): boolean {
  return /SE\/30|II/i.test(model);
}

// Drop the leading "Macintosh " for compact display (status bar etc.).
export function shortModel(model: string): string {
  return model.replace(/^Macintosh\s+/i, '');
}

// Default new-machine config — populates the Configuration slide form on
// first render. Matches the prototype's default option for each select.
export const DEFAULT_CONFIG: MachineConfig = {
  model: 'Macintosh Plus',
  vrom: '(auto)',
  ram: '4 MB',
  floppies: [],
  hd: 'hd1.img',
  cd: '(none)',
};
