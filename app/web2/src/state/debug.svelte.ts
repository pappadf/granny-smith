// Reactive state for the Debug panel view. UI-only — not persisted.
// Section expansion lives here (Phase 7 will tee this to OPFS).

export type MmuSubtab = 'state' | 'translate' | 'map' | 'descriptors';
export type MemoryMode = 'logical' | 'physical';

interface DebugState {
  currentPc: number;
  memoryAddress: number;
  memoryMode: MemoryMode;
  mmuSubtab: MmuSubtab;
  mmuTransAddr: number;
  mmuSupervisor: boolean;
  sections: {
    registers: boolean;
    fpu: boolean;
    memory: boolean;
    mmu: boolean;
    breakpoints: boolean;
    watchpoints: boolean;
    callstack: boolean;
  };
  /** Last-rendered register values keyed by name (e.g. 'd0', 'pc'). Used
   *  by RegistersSection to flash changed values for ~800 ms after a
   *  Step. */
  registersPrev: Record<string, number>;
  /** Monotonic counter bumped after every Step Into / Step Over. The
   *  Debug panes watch this in their $effects in addition to
   *  machine.status — stepping while paused doesn't change run-state
   *  (paused → paused, no onRunStateChange push), so we need a
   *  reactive signal of "PC moved, re-fetch" to trigger refreshes. */
  refreshGen: number;
}

export const debug: DebugState = $state({
  currentPc: 0,
  memoryAddress: 0x00000000,
  memoryMode: 'logical',
  mmuSubtab: 'state',
  mmuTransAddr: 0x00400000,
  mmuSupervisor: false,
  // Only the Disassembly pane (rendered above the sections, not a
  // CollapsibleSection) is visible by default; every collapsible
  // section starts closed so first-paint stays compact. The user's
  // expansion state is persisted in localStorage from then on.
  sections: {
    registers: false,
    fpu: false,
    memory: false,
    mmu: false,
    breakpoints: false,
    watchpoints: false,
    callstack: false,
  },
  registersPrev: {},
  refreshGen: 0,
});

export function bumpDebugRefresh(): void {
  debug.refreshGen++;
}

export function toggleSection(name: keyof DebugState['sections']): void {
  debug.sections[name] = !debug.sections[name];
}

export function setSection(name: keyof DebugState['sections'], open: boolean): void {
  debug.sections[name] = open;
}

// Cross-surface verb: from a disasm row's right-click → MMU section
// expands, Translate sub-tab activates, the address is seeded, and the
// section scrolls into view.
export function inspectMmuWalk(addr: number): void {
  debug.mmuTransAddr = addr >>> 0;
  debug.mmuSubtab = 'translate';
  debug.sections.mmu = true;
}

// From a register's right-click → Memory section expands and seeds.
export function inspectMemoryAt(addr: number): void {
  debug.memoryAddress = addr >>> 0;
  debug.sections.memory = true;
}

// Phase 7 will surface this via OPFS persistence; for now it's just a
// session reset hook used by tests + by the panel-position change
// handler if we want to reset orientations.
export function resetDebugSections(): void {
  debug.sections = {
    registers: false,
    fpu: false,
    memory: false,
    mmu: false,
    breakpoints: false,
    watchpoints: false,
    callstack: false,
  };
}
