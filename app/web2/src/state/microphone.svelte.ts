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

export interface AudioInputDevice {
  id: string;
  label: string;
}

interface MicrophoneState {
  // The user's master toggle (the toolbar button).
  enabled: boolean;
  // The guest has sound input running (Singer pSndInEn).
  guestActive: boolean;
  // A MediaStreamTrack is attached and samples are flowing.
  live: boolean;
  // Selectable capture devices, and the one the user picked ('' = system
  // default). Without this the machine is stuck with whatever getUserMedia
  // considers default, and that is routinely NOT the user's microphone: a
  // dock's unplugged headset jack, an HDMI input, a monitor source. Such a
  // device is live and well-behaved — it simply delivers its own dither at
  // about -80 dBFS forever, which is indistinguishable from a broken
  // capture path anywhere downstream, and cost several rounds of chasing
  // bugs in the emulator that were never there.
  devices: AudioInputDevice[];
  deviceId: string;
}

export const microphone: MicrophoneState = $state({
  enabled: false,
  guestActive: false,
  live: false,
  devices: [],
  deviceId: '',
});

// Shared-heap transport, announced by em_audio_in.c at startup.
// Header Int32 layout: [0] connected, [1] wr, [2] rd, [3] rate,
// [4] underruns, [5] overruns, [6] js_rms, [7] js_peak, then char
// label[64] at byte 32. The int16 ring follows the 96-byte header.
let shmPtr = 0;
let ringLen = 0;
const HDR_BYTES = 96;
const LABEL_OFF = 32; // char label[64] — the chosen capture device
const LABEL_MAX = 63;

let stream: MediaStream | null = null;
let audioCtx: AudioContext | null = null;
let node: AudioWorkletNode | ScriptProcessorNode | null = null;
let source: MediaStreamAudioSourceNode | null = null;

// --- Module callbacks (attached in bus/emulator.ts) ------------------------

// em_audio_in_init passes a third argument, the codec rate the C side would
// prefer. It is deliberately NOT used to construct the AudioContext — see
// buildGraph — so it is not taken as a parameter here at all; the rate
// actually negotiated travels the other way instead, via resetRing.
export function onAudioInReady(ptr: number, len: number): void {
  shmPtr = ptr;
  ringLen = len;
}

// Expose the counters for the e2e spec (and for anyone debugging in the
// console). Cheap, and the alternative is guessing at which layer is dead.
if (typeof globalThis !== 'undefined') {
  (globalThis as unknown as { __micStats?: () => unknown }).__micStats = () => micStats();
  (globalThis as unknown as { __micLive?: () => unknown }).__micLive = () => ({
    enabled: microphone.enabled,
    guestActive: microphone.guestActive,
    live: microphone.live,
    ctx: audioCtx ? audioCtx.state + '@' + audioCtx.sampleRate : 'none',
    node: node ? node.constructor.name : 'none',
  });
}

export function onAudioInState(active: boolean): void {
  const started = active && !microphone.guestActive;
  microphone.guestActive = active;
  // The guest has started recording. If no microphone is connected it will
  // capture the codec's own noise floor, and its recorder gains that up into
  // full-scale white noise — a result indistinguishable from a broken
  // capture path, with nothing anywhere to say the microphone was simply
  // never connected. Say so. The connection stays a deliberate user act
  // (attaching a microphone because the guest asked would be exactly the
  // wrong default), but silence about it is not a defensible one.
  if (started && !microphone.enabled && machine.audioIn) {
    showNotification(
      'The Mac is recording, but no microphone is connected — click the microphone button in the toolbar',
      'warning',
    );
  }
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
  Atomics.store(heap.i32, hdr + 6, 0); // js_rms
  Atomics.store(heap.i32, hdr + 7, 0); // js_peak
}

// Publish the capture device's name where the emulator's level meter prints
// it. getUserMedia takes the SYSTEM DEFAULT input, which on a machine with
// several is often not the microphone the user means — and a wrong-but-live
// device delivers exactly what a broken ring would: full-rate quanta of
// near-silence. The meter cannot distinguish those; the device name can.
function publishLabel(text: string): void {
  const heap = getModuleHeap();
  if (!heap || !shmPtr) return;
  const bytes = new TextEncoder().encode(text);
  const n = Math.min(bytes.length, LABEL_MAX);
  for (let i = 0; i < n; i++) heap.u8[shmPtr + LABEL_OFF + i] = bytes[i];
  heap.u8[shmPtr + LABEL_OFF + n] = 0;
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
  // Measure what the WORKLET handed us, before our conversion or the ring:
  // the single number that says whether the browser is capturing at all.
  let sum = 0;
  let peak = 0;
  for (let i = 0; i < n; i++) {
    const v = block[i];
    const s = v < -1 ? -32768 : v > 1 ? 32767 : Math.round(v * 32767);
    heap.i16[base + ((wr + i) % ringLen)] = s;
    sum += s * s;
    if (Math.abs(s) > peak) peak = Math.abs(s);
  }
  if (n) {
    Atomics.store(heap.i32, hdr + 6, Math.round(Math.sqrt(sum / n)));
    Atomics.store(heap.i32, hdr + 7, peak);
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

async function buildGraph(s: MediaStream): Promise<boolean> {
  // Use the browser's NATIVE rate. Asking for the codec's 24 kHz instead
  // looks tidier — it keeps the C side at 1:1 — but a real capture device
  // routed into a context whose sampleRate does not match the track's
  // delivers SILENCE: the graph runs, the worklet is pulled, full quanta
  // arrive, and every sample is ~0. Chromium's fake device adopts whatever
  // rate is requested, so an e2e against it passes while every real
  // microphone fails, which is exactly how this survived being "verified".
  // The rate actually negotiated is published to the C side, which resamples.
  const ctx = new AudioContext();
  try {
    await ctx.resume();
  } catch {
    /* resume is best-effort; the click that got us here is the gesture */
  }
  audioCtx = ctx;
  source = ctx.createMediaStreamSource(s);
  resetRing(ctx.sampleRate);

  try {
    // Page-relative, like the icon sprite: an origin-rooted path 404s under
    // deploy subpaths, and a blob: URL is rejected outright under the
    // cross-origin isolation SharedArrayBuffer needs.
    await ctx.audioWorklet.addModule('gs-mic-tap.js');
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
  const audio: MediaTrackConstraints = {
    channelCount: 1,
    // See the header: browser voice processing must stay out of the way.
    echoCancellation: false,
    noiseSuppression: false,
    autoGainControl: false,
  };
  // `exact`, deliberately: a device the user chose explicitly must not be
  // silently swapped for the default when it is busy or unplugged. Failing
  // loudly and falling back below is honest; capturing the wrong device is
  // the failure this whole selection exists to prevent.
  if (microphone.deviceId) audio.deviceId = { exact: microphone.deviceId };
  try {
    return await navigator.mediaDevices.getUserMedia({ audio, video: false });
  } catch {
    if (!microphone.deviceId) return null;
    // The chosen device is gone (undocked, unplugged). Say so, drop back to
    // the default rather than leaving the user with no microphone at all.
    showNotification('Selected microphone unavailable — using the system default', 'warning');
    microphone.deviceId = '';
    try {
      return await navigator.mediaDevices.getUserMedia({
        audio: { ...audio, deviceId: undefined },
        video: false,
      });
    } catch {
      return null;
    }
  }
}

// Enumerate capture devices. Labels are blank until permission has been
// granted at least once, so this is worth re-running after a connect.
export async function refreshAudioInputs(): Promise<void> {
  try {
    const devs = await navigator.mediaDevices.enumerateDevices();
    microphone.devices = devs
      .filter((d) => d.kind === 'audioinput')
      .map((d, i) => ({ id: d.deviceId, label: d.label || `Input ${i + 1}` }));
  } catch {
    microphone.devices = [];
  }
}

// Switch capture device. Re-acquires immediately when a stream is attached,
// so the change is audible at once rather than at the next recording.
export async function setMicrophoneDevice(id: string): Promise<void> {
  if (id === microphone.deviceId) return;
  microphone.deviceId = id;
  if (stream) {
    stopStream();
    await syncStream();
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
//
// Serialised, and it matters: the two callers — the user's toggle and the
// guest's pSndInEn gate — routinely fire within a tick of each other, and
// two overlapping runs each build a graph, then one's stopStream() closes
// the other's AudioContext mid-construction. That surfaced as
// "Construction of ScriptProcessorNode is not useful when context is
// closed", a failed buildGraph, and the toggle switching itself back off.
let syncing: Promise<void> | null = null;
async function syncStream(): Promise<void> {
  // Coalesce: a call arriving mid-flight waits for the in-flight one, then
  // re-reconciles, so the final state always matches the latest intent.
  while (syncing) {
    const inFlight = syncing;
    await inFlight;
    if (syncing === inFlight) break;
  }
  syncing = syncStreamInner();
  try {
    await syncing;
  } finally {
    syncing = null;
  }
}

async function syncStreamInner(): Promise<void> {
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
    const t0 = s.getAudioTracks()[0];
    if (t0) {
      const st = t0.getSettings() as Record<string, unknown>;
      publishLabel(`${t0.label}${t0.muted ? ' [MUTED]' : ''}`);
      console.log(
        `[gs mic] track "${t0.label}" enabled=${t0.enabled} muted=${t0.muted} state=${t0.readyState}`,
        st,
      );
    }
    // Permission is granted now, so the device labels are finally readable;
    // populate the picker from here rather than at startup.
    await refreshAudioInputs();
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
