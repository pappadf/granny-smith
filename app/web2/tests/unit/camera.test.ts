// The camera's stream reconciliation is single-flight (F-41): overlapping
// triggers acquire one device, a stream the intent no longer wants is
// released, and a toggle-off during play() leaves nothing running (N-59).
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';

vi.mock('@/bus/emulator', () => ({
  gsEval: vi.fn(async () => null),
  getModuleHeap: () => null,
}));

import { camera, onVideoInState, setCameraEnabled } from '@/state/camera.svelte';

interface FakeTrack {
  stopped: boolean;
  stop(): void;
}
let tracks: FakeTrack[] = [];
let pending: ((s: unknown) => void)[] = [];
let playGate: Promise<void> | null = null;

function fakeStream() {
  const t: FakeTrack = {
    stopped: false,
    stop() {
      this.stopped = true;
    },
  };
  tracks.push(t);
  return { getTracks: () => [t] };
}

beforeEach(async () => {
  tracks = [];
  pending = [];
  playGate = null;
  Object.defineProperty(navigator, 'mediaDevices', {
    configurable: true,
    value: {
      getUserMedia: vi.fn(() => new Promise((resolve) => pending.push(resolve))),
    },
  });
  vi.spyOn(HTMLMediaElement.prototype, 'play').mockImplementation(async () => {
    if (playGate) await playGate;
  });
  Object.defineProperty(HTMLMediaElement.prototype, 'srcObject', {
    configurable: true,
    set() {},
    get() {
      return null;
    },
  });
  camera.enabled = false;
  camera.guestActive = false;
  camera.live = false;
});

afterEach(async () => {
  // Leave no stream behind for the next test.
  camera.enabled = false;
  onVideoInState(false);
  await flush();
  vi.restoreAllMocks();
});

const flush = () => new Promise((r) => setTimeout(r, 0));
const gum = () => navigator.mediaDevices.getUserMedia as unknown as ReturnType<typeof vi.fn>;

describe('camera stream reconciliation', () => {
  it('overlapping triggers acquire one device', async () => {
    camera.enabled = true;
    onVideoInState(true);
    onVideoInState(true); // a second push while the first waits on permission
    await flush();
    expect(gum()).toHaveBeenCalledTimes(1);
    pending[0](fakeStream());
    await flush();
    await flush();
    expect(gum()).toHaveBeenCalledTimes(1);
    expect(camera.live).toBe(true);
    expect(tracks.filter((t) => !t.stopped).length).toBe(1);
  });

  it('a stream that arrives after the guest stopped is released', async () => {
    camera.enabled = true;
    onVideoInState(true);
    await flush();
    onVideoInState(false); // queued behind the in-flight acquire
    pending[0](fakeStream());
    await flush();
    await flush();
    expect(tracks[0].stopped).toBe(true);
    expect(camera.live).toBe(false);
  });

  it('a toggle-off during play() leaves nothing running', async () => {
    let releasePlay!: () => void;
    playGate = new Promise<void>((r) => (releasePlay = r));
    camera.enabled = true;
    onVideoInState(true);
    await flush();
    pending[0](fakeStream());
    await flush();
    await setCameraEnabled(false); // stopStream() while play() is pending
    releasePlay();
    await flush();
    await flush();
    expect(tracks[0].stopped).toBe(true);
    expect(camera.live).toBe(false);
  });
});
