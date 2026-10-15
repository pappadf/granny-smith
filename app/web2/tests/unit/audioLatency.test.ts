// ?audio_latency=<ms> -> the worklet's latency target (src/audio/latency.ts).

import { describe, it, expect } from 'vitest';
import { audioTargetLatencyFromQuery } from '@/audio/latency';

describe('audioTargetLatencyFromQuery', () => {
  it('is undefined without the parameter', () => {
    expect(audioTargetLatencyFromQuery('')).toBeUndefined();
    expect(audioTargetLatencyFromQuery('?speed=turbo')).toBeUndefined();
    expect(audioTargetLatencyFromQuery('?audio_latency=')).toBeUndefined();
  });

  it('converts milliseconds to seconds', () => {
    expect(audioTargetLatencyFromQuery('?audio_latency=50')).toBeCloseTo(0.05);
    expect(audioTargetLatencyFromQuery('?x=1&audio_latency=120')).toBeCloseTo(0.12);
  });

  it('refuses values outside 20-250 ms and garbage', () => {
    expect(audioTargetLatencyFromQuery('?audio_latency=5')).toBeUndefined();
    expect(audioTargetLatencyFromQuery('?audio_latency=1000')).toBeUndefined();
    expect(audioTargetLatencyFromQuery('?audio_latency=fast')).toBeUndefined();
  });
});
