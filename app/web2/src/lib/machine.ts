// Pure helpers for machine config / model identification.

import type { MachineConfig } from '@/bus/types';

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
