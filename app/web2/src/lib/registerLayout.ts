// How the Debug view groups a core's register file (a frame's `regs`, by
// the core's own names).  Per-architecture layouts; anything the core
// reports that a layout does not name lands in a trailing group, and an
// architecture with no layout at all gets one generic grid — so a new core
// renders without a table, never as blanks.  Shared by the main CPU's
// Registers section and the auxiliary cores' sections.

export interface RegisterGroup {
  title: string;
  names: string[];
}

const LAYOUTS: Record<string, RegisterGroup[]> = {
  m68k: [
    { title: 'Data', names: ['d0', 'd1', 'd2', 'd3', 'd4', 'd5', 'd6', 'd7'] },
    { title: 'Address', names: ['a0', 'a1', 'a2', 'a3', 'a4', 'a5', 'a6', 'a7'] },
    { title: 'Control', names: ['pc', 'sr', 'usp', 'ssp'] },
  ],
  ppc: [
    { title: 'General', names: Array.from({ length: 32 }, (_, i) => `r${i}`) },
    {
      title: 'Special',
      names: ['pc', 'lr', 'ctr', 'cr', 'xer', 'msr', 'srr0', 'srr1', 'mq'],
    },
  ],
  dsp3210: [
    { title: 'General', names: Array.from({ length: 22 }, (_, i) => `r${i + 1}`) },
    { title: 'Control', names: ['pc', 'ps', 'emr', 'pcw', 'dauc', 'ctr'] },
  ],
};

// Registers narrower than 32 bits, by architecture, formatted at their width.
const WIDTHS: Record<string, Record<string, number>> = {
  m68k: { sr: 16 },
  dsp3210: { ps: 16, emr: 16, pcw: 16, dauc: 8, ctr: 8 },
};

export function registerGroups(arch: string, values: Record<string, number>): RegisterGroup[] {
  const layout = LAYOUTS[arch] ?? [];
  const named = new Set(layout.flatMap((g) => g.names));
  const out = layout
    .map((g) => ({ title: g.title, names: g.names.filter((n) => n in values) }))
    .filter((g) => g.names.length);
  const rest = Object.keys(values).filter((n) => !named.has(n));
  if (rest.length) out.push({ title: layout.length ? 'Other' : 'Registers', names: rest });
  return out;
}

// Bits in a register, for formatting (32 unless the layout says narrower).
export function registerBits(arch: string, name: string): number {
  return WIDTHS[arch]?.[name] ?? 32;
}

// A register value as upper-case hex at its width.
export function fmtRegister(arch: string, name: string, value: number): string {
  const digits = registerBits(arch, name) / 4;
  const mask = digits >= 8 ? 0xffffffff : (1 << (digits * 4)) - 1;
  return ((value & mask) >>> 0).toString(16).toUpperCase().padStart(digits, '0');
}
