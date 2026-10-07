// lib/urlConfig.ts: a URL boot's configuration parameters as document edits.
import { describe, it, expect } from 'vitest';
import type { MachineProfile } from '@/bus/profile';
import { parseUrlMediaParams } from '@/bus/urlMedia';
import { applyUrlConfig, isUrlConfigParam } from '@/lib/urlConfig';
import { tree, TREE_IDS } from '../helpers/configTree';

const profile = (id: string) => tree(id) as unknown as MachineProfile;

// The edits a query string asks for, applied to the default configuration.
function apply(id: string, qs: string, base: Record<string, unknown> | null = null) {
  return applyUrlConfig(profile(id), base, parseUrlMediaParams(new URLSearchParams(qs)).settings);
}

type Doc = {
  options: Record<string, string>;
  displays: Record<string, { monitor: string; mode?: string }>;
};

describe('URL configuration parameters', () => {
  it('no option of any model is a name the page reads for something else', () => {
    for (const id of TREE_IDS)
      for (const o of profile(id).options)
        expect(isUrlConfigParam(o.id), `${id}: ${o.id}`).toBe(true);
  });

  it('media and page parameters are not configuration edits', () => {
    const p = parseUrlMediaParams(
      new URLSearchParams(
        'rom=/r&HD0=/h&fd1=/f&model=iix&speed=turbo&skin=x&config=e30&addressing=32',
      ),
    );
    expect(p.settings.options).toEqual([{ id: 'addressing', value: '32', name: 'addressing' }]);
  });

  it('no edits: the document is left as it was', () => {
    expect(apply('iix', '')).toEqual({ config: null, warnings: [] });
  });

  it('any option of the model, by value id or label', () => {
    const { config, warnings } = apply('iix', 'Addressing=32-bit&appletalk=inactive&ram=32MB');
    expect(warnings).toEqual([]);
    expect((config as Doc).options).toMatchObject({
      addressing: '32',
      appletalk: 'inactive',
      memory: '32768',
    });
  });

  it('the ImageWriter, by value id or label', () => {
    expect((apply('plus', 'imagewriter=imagewriter2').config as Doc).options.imagewriter).toBe(
      'imagewriter2',
    );
    expect((apply('lisa', 'ImageWriter=ImageWriter').config as Doc).options.imagewriter).toBe(
      'imagewriter',
    );
  });

  it('a value the option does not offer is reported and left out', () => {
    const { config, warnings } = apply('iix', 'addressing=33');
    expect(config).toBeNull();
    expect(warnings).toEqual(['addressing=33: Addressing is one of 24-bit, 32-bit']);
  });

  it('an option the model does not have is skipped quietly', () => {
    // The Plus runs 24-bit only: its tree has no addressing option.
    expect(apply('plus', 'addressing=32&utm_source=x')).toEqual({ config: null, warnings: [] });
  });

  it('monitor= and mode= edit the connected display', () => {
    const { config, warnings } = apply('iix', 'monitor=21in_rgb&mode=1152x870x8');
    expect(warnings).toEqual([]);
    expect((config as Doc).displays.nubus_9).toEqual({ monitor: '21in_rgb', mode: '1152x870x8' });
  });

  it('a size alone is the deepest mode, found on whichever monitor offers it', () => {
    const { config } = apply('iix', 'mode=640x870');
    expect((config as Doc).displays.nubus_9).toEqual({
      monitor: '15in_portrait',
      mode: '640x870x8',
    });
  });

  it('a mode the monitor does not offer is reported', () => {
    const { config, warnings } = apply('iici', 'mode=1152x870x8');
    expect(config).toBeNull();
    expect(warnings[0]).toMatch(
      /^mode=1152x870x8: .* offers 640x480x1, 640x480x2, 640x480x4, 640x480x8$/,
    );
  });

  it('display= moves the monitor to another device', () => {
    const base = {
      cards: [{ slot: 'nubus_c', card: 'mdc_8_24', options: {} }],
      displays: { builtin: { monitor: '13in_rgb' }, nubus_c: { monitor: 'none' } },
    };
    const { config } = apply('iici', 'display=nubus_c&monitor=21in_rgb', base);
    expect((config as Doc).displays).toEqual({
      builtin: { monitor: 'none' },
      nubus_c: { monitor: '21in_rgb' },
    });
  });

  it('a config= keeps the nodes it left out out, and its own values', () => {
    const base = { model: 'iix', options: { appletalk: 'inactive' }, storage: [] };
    const { config } = apply('iix', 'addressing=32', base);
    expect(config).toMatchObject({
      options: { appletalk: 'inactive', addressing: '32' },
      storage: [],
    });
    expect(config).not.toHaveProperty('startup');
    expect(base.options).toEqual({ appletalk: 'inactive' });
  });
});
