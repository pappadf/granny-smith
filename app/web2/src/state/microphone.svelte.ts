// Microphone state — the browser side of the AV audio-in path
// (C side: src/platform/wasm/em_audio_in.c; the guest-side contract is
// docs/machines/av/singer.md).
//
// Transport: em_audio_in.c owns a lock-free SPSC ring + atomic header in the
// shared wasm heap and announces its address once via Module.onAudioInReady.
// This module writes captured mono int16 into the ring and bumps `wr`; the
// emulator worker reads and bumps `rd`. Audio cannot use the camera's
// latest-wins slot flip — the guest pulls whole 10 ms half-buffers on the
// Singer's frame cadence while the browser produces on its own, so the two
// rates have to be decoupled by a queue.
//
// Lifecycle & privacy: the browser's recording indicator is lit only while
// the guest is genuinely listening. `enabled` is the user's master toggle
// (the click is also the user gesture getUserMedia wants); `guestActive`
// follows pSndInEn via Module.onAudioInState. The MediaStreamTrack is
// attached only while BOTH hold — the same discipline as the camera.
//
// Capture settings matter more than they look. The browser's own
// echoCancellation / noiseSuppression / autoGainControl are all turned OFF
// on purpose: they are voice-call processing, and an AGC in front of the
// emulator is precisely the defect that made recorded test assets unusable
// (levels 6-12 dB hot with the dynamics flattened, which Apple's recognizer
// rejects — see local/gs-docs/dsp3210-plaintalk/sr-test-audio-assets.md §2).
// The C side then applies the PlainTalk microphone's own characteristics.

import { getModuleHeap } from '@/bus/emulator';
import { gsEval } from '@/bus/emulator';
import { machine } from './machine.svelte';
import { showNotification } from './toasts.svelte';

interface MicrophoneState {
  // The user's master toggle (the toolbar button).
  enabled: boolean;
  // The guest has sound input running (Singer pSndInEn).
  guestActive: boolean;
  // A MediaStreamTrack is attached and samples are flowing.
  live: boolean;
}

export const microphone: MicrophoneState = $state({
  enabled: false,
  guestActive: false,
  live: false,
});

// Shared-heap transport, announced by em_audio_in.c at startup.
// Header Int32 layout: [0] connected, [1] wr, [2] rd, [3] rate,
// [4] underruns, [5] overruns. The int16 ring follows the 24-byte header.
let shmPtr = 0;
let ringLen = 0;
let wantRate = 24000;
const HDR_BYTES = 24;

let stream: MediaStream | null = null;
let audioCtx: AudioContext | null = null;
let node: AudioWorkletNode | ScriptProcessorNode | null = null;
let source: MediaStreamAudioSourceNode | null = null;

// --- Module callbacks (attached in bus/emulator.ts) ------------------------

export function onAudioInReady(ptr: number, len: number, rate: number): void {
  shmPtr = ptr;
  ringLen = len;
  wantRate = rate;
}

export function onAudioInState(active: boolean): void {
  microphone.guestActive = active;
  void syncStream();
}

// --- Header helpers ---------------------------------------------------------

function setConnected(v: boolean): void {
  const heap = getModuleHeap();
  if (!heap || !shmPtr) return;
  Atomics.store(heap.i32, shmPtr >> 2, v ? 1 : 0);
}

// Drop anything queued and tell the C side what rate is actually arriving.
function resetRing(rate: number): void {
  const heap = getModuleHeap();
  if (!heap || !shmPtr) return;
  const hdr = shmPtr >> 2;
  Atomics.store(heap.i32, hdr + 1, 0); // wr
  Atomics.store(heap.i32, hdr + 2, 0); // rd
  Atomics.store(heap.i32, hdr + 3, rate | 0);
  Atomics.store(heap.i32, hdr + 4, 0); // underruns
  Atomics.store(heap.i32, hdr + 5, 0); // overruns
}

// Live transport counters, for the toolbar tooltip. Without these, "the
// capture path is dead" and "the audio is wrong" are indistinguishable from
// outside — the guest amplifies silence into full-scale white noise, so both
// sound the same.
export function micStats(): {
  ptr: number;
  rate: number;
  produced: number;
  consumed: number;
  underruns: number;
  overruns: number;
} {
  const heap = getModuleHeap();
  if (!heap || !shmPtr)
    return { ptr: shmPtr, rate: 0, produced: 0, consumed: 0, underruns: 0, overruns: 0 };
  const hdr = shmPtr >> 2;
  return {
    ptr: shmPtr,
    rate: Atomics.load(heap.i32, hdr + 3),
    produced: Atomics.load(heap.i32, hdr + 1),
    consumed: Atomics.load(heap.i32, hdr + 2),
    underruns: Atomics.load(heap.i32, hdr + 4),
    overruns: Atomics.load(heap.i32, hdr + 5),
  };
}

// Append one block of mono samples. The producer writes ONLY `wr` — it must
// never touch `rd`, or the ring has two writers on the same index and the
// lock-free claim is false. A producer that gets ahead is the consumer's
// problem to notice, and it does (em_audio_in.c drops its own backlog).
function pushSamples(block: Float32Array): void {
  const heap = getModuleHeap();
  if (!heap || !shmPtr || !ringLen) return;
  const hdr = shmPtr >> 2;
  let wr = Atomics.load(heap.i32, hdr + 1);
  const n = Math.min(block.length, ringLen);
  const base = (shmPtr + HDR_BYTES) >> 1; // int16 index of ring[0]
  for (let i = 0; i < n; i++) {
    const v = block[i];
    const s = v < -1 ? -32768 : v > 1 ? 32767 : Math.round(v * 32767);
    heap.i16[base + ((wr + i) % ringLen)] = s;
  }
  wr += n;
  Atomics.store(heap.i32, hdr + 1, wr);
}

// --- Capture graph ----------------------------------------------------------

// A tiny worklet: hand each render quantum's mono channel to the main thread.
// Copying through a port message keeps the ring writer on one thread, which
// is what makes the single-producer claim on the C side true.
// A tap must reach the destination to be rendered at all; a zeroed gain
// gets it there without the user hearing themselves.
function muteToDestination(ctx: AudioContext): GainNode {
  const mute = ctx.createGain();
  mute.gain.value = 0;
  mute.connect(ctx.destination);
  return mute;
}

const WORKLET_SRC = `
class GsMicTap extends AudioWorkletProcessor {
  process(inputs) {
    const ch = inputs[0] && inputs[0][0];
    if (ch && ch.length) this.port.postMessage(ch.slice(0));
    return true;   // outputs are left silent; this node exists only to tap
  }
}
registerProcessor('gs-mic-tap', GsMicTap);
`;

async function buildGraph(s: MediaStream): Promise<boolean> {
  // Ask the browser to resample to the codec rate: its resampler is better
  // than anything worth writing here, and it keeps the C side at 1:1.
  const ctx = new AudioContext({ sampleRate: wantRate });
  try {
    await ctx.resume();
  } catch {
    /* resume is best-effort; the click that got us here is the gesture */
  }
  audioCtx = ctx;
  source = ctx.createMediaStreamSource(s);
  resetRing(ctx.sampleRate);

  try {
    const url = URL.createObjectURL(new Blob([WORKLET_SRC], { type: 'application/javascript' }));
    await ctx.audioWorklet.addModule(url);
    URL.revokeObjectURL(url);
    const wn = new AudioWorkletNode(ctx, 'gs-mic-tap', {
      numberOfInputs: 1,
      numberOfOutputs: 1,
      outputChannelCount: [1],
    });
    wn.port.onmessage = (e: MessageEvent<Float32Array>) => pushSamples(e.data);
    source.connect(wn);
    // A node with no path to the destination is NOT part of the rendering
    // graph, so its process() is never called and not one sample is
    // captured. Built with numberOfOutputs: 0 and left dangling, this tap
    // silently produced nothing — and because the guest's recorder gains up
    // hard, the silence came back as full-scale white noise rather than as
    // silence, which is a much more misleading symptom. Route it to the
    // destination through a zeroed gain, exactly as the fallback below
    // already did for the same reason.
    wn.connect(muteToDestination(ctx));
    node = wn;
    return true;
  } catch {
    // Fall back for browsers without AudioWorklet. ScriptProcessorNode is
    // deprecated but still the only universally available tap.
    try {
      const sp = ctx.createScriptProcessor(1024, 1, 1);
      sp.onaudioprocess = (e) => pushSamples(e.inputBuffer.getChannelData(0));
      source.connect(sp);
      sp.connect(muteToDestination(ctx));
      node = sp;
      return true;
    } catch {
      return false;
    }
  }
}

async function acquireStream(): Promise<MediaStream | null> {
  try {
    return await navigator.mediaDevices.getUserMedia({
      audio: {
        channelCount: 1,
        // See the header: browser voice processing must stay out of the way.
        echoCancellation: false,
        noiseSuppression: false,
        autoGainControl: false,
      },
      video: false,
    });
  } catch {
    return null;
  }
}

function stopStream(): void {
  if (node) {
    node.disconnect();
    if ('port' in node) node.port.onmessage = null;
    else (node as ScriptProcessorNode).onaudioprocess = null;
    node = null;
  }
  source?.disconnect();
  source = null;
  if (audioCtx) {
    void audioCtx.close().catch(() => {});
    audioCtx = null;
  }
  if (stream) {
    for (const t of stream.getTracks()) t.stop();
    stream = null;
  }
  microphone.live = false;
}

// Reconcile the physical microphone with (enabled && guestActive).
async function syncStream(): Promise<void> {
  const want = microphone.enabled && microphone.guestActive;
  if (want && !stream) {
    const s = await acquireStream();
    if (!s) {
      if (microphone.enabled) {
        showNotification('Microphone unavailable — permission denied or no device', 'warning');
        await setMicrophoneEnabled(false);
      }
      return;
    }
    // The toggle may have flipped while the permission prompt was up.
    if (!(microphone.enabled && microphone.guestActive)) {
      for (const t of s.getTracks()) t.stop();
      return;
    }
    stream = s;
    if (!(await buildGraph(s))) {
      showNotification('Microphone unavailable — no audio capture path', 'warning');
      stopStream();
      await setMicrophoneEnabled(false);
      return;
    }
    microphone.live = true;
  } else if (!want && stream) {
    stopStream();
  }
}

// --- The master toggle ------------------------------------------------------

// User intent: connect/disconnect the microphone from the guest's sound
// input. Connecting selects the `host` source on machine.audioin (the Singer
// then reports a microphone present); the device itself attaches only while
// the guest is actually recording (syncStream).
export async function setMicrophoneEnabled(on: boolean): Promise<void> {
  microphone.enabled = on;
  if (on) {
    // `connected` goes true on the toggle, not on the first sample: the
    // guest reads it as the mic-present sense to decide whether to START
    // recording, and samples only flow once recording runs — gating it on
    // samples would deadlock. Until the first block lands the ring is empty
    // and the seam reports "no source", so the guest records the codec's own
    // noise floor rather than stale audio.
    setConnected(true);
    await gsEval('machine.audioin.source', ['host']);
    // Prime the permission prompt under the click's user gesture, then
    // release the device again if the guest is not recording yet.
    await syncStream();
    if (!microphone.guestActive && microphone.enabled) {
      const s = await acquireStream();
      if (s) {
        for (const t of s.getTracks()) t.stop();
      } else {
        showNotification('Microphone unavailable — permission denied or no device', 'warning');
        await setMicrophoneEnabled(false);
      }
    }
  } else {
    setConnected(false);
    stopStream();
    await gsEval('machine.audioin.source', ['none']);
  }
}

// Re-apply the user's microphone intent after a (re)boot: machine.audioin
// resets to `none` with the machine, while `microphone.enabled` is UI intent
// that survives it. Called from initEmulator once capabilities are known.
export async function reapplyMicrophoneSource(): Promise<void> {
  if (!machine.audioIn) {
    // The new machine has no audio input: drop the stream and the intent.
    if (microphone.enabled) await setMicrophoneEnabled(false);
    microphone.guestActive = false;
    return;
  }
  if (microphone.enabled) {
    setConnected(true);
    await gsEval('machine.audioin.source', ['host']);
  }
}
