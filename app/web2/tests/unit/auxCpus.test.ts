import { describe, it, expect } from 'vitest';
import { parseAuxCpus } from '@/bus/emulator';

// capabilities.aux_cpus -> machine.auxCpus (F-08: exported, read by nothing).
describe('parseAuxCpus', () => {
  it('reads the AV DSP entry', () => {
    expect(parseAuxCpus([{ name: 'dsp', arch: 'dsp3210', freq: 66666667 }])).toEqual([
      { name: 'dsp', arch: 'dsp3210', freq: 66666667 },
    ]);
  });

  it('is empty on a machine without auxiliary cores, or a malformed field', () => {
    expect(parseAuxCpus([])).toEqual([]);
    expect(parseAuxCpus(undefined)).toEqual([]);
    expect(parseAuxCpus('dsp')).toEqual([]);
  });

  it('drops a name that is not one identifier (it becomes a path segment)', () => {
    expect(
      parseAuxCpus([
        { name: 'cpu.mmu', arch: 'x' },
        { name: 'dsp2', arch: 'y' },
      ]),
    ).toEqual([{ name: 'dsp2', arch: 'y', freq: 0 }]);
  });
});
