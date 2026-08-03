// Camera state — the browser side of the AV video-in path
// (proposal-av-video-in.md §2.3; C side: src/platform/wasm/em_camera.c).
//
// Transport: em_camera.c owns a static double-buffered frame slot pair +
// atomic header in the shared wasm heap and announces its address once via
// Module.onVideoInReady. This module writes each decoded webcam frame into
// the NON-active slot through the live heap views and then flips `active`;
// the emulator worker copies out of the active slot at field cadence, so
// tearing is impossible and staleness is at most one frame.
//
// Lifecycle & privacy: the camera light is on only while the guest actually
// captures. `enabled` is the user's master toggle (the click is also the
// user gesture getUserMedia wants); `guestActive` follows the VDC clock via
// Module.onVideoInState. The MediaStreamTrack is attached only while BOTH
// hold; a permission-priming getUserMedia runs at enable time and is
// stopped immediately when the guest is not capturing.

import { gsEval, getModuleHeap } from '@/bus/emulator';
import { machine } from './machine.svelte';
import { showNotification } from './toasts.svelte';

interface CameraState {
  // The user's master toggle (the toolbar button).
  enabled: boolean;
  // The guest's capture engine is running (VDC clock ungated).
  guestActive: boolean;
  // A MediaStreamTrack is attached and frames are being delivered.
  live: boolean;
}

export const camera: CameraState = $state({
  enabled: false,
  guestActive: false,
  live: false,
});

// Shared-heap transport, announced by em_camera.c at startup.
// Header Int32 layout: [0] connected, [1] active slot (-1 none), [2] seq.
let shmPtr = 0;
let shmW = 640;
let shmH = 480;
const HDR_BYTES = 16;

let stream: MediaStream | null = null;
let videoEl: HTMLVideoElement | null = null;
let canvasEl: HTMLCanvasElement | null = null;
let pumpStop: (() => void) | null = null;

// --- Module callbacks (attached in bus/emulator.ts) ------------------------

export function onVideoInReady(ptr: number, w: number, h: number): void {
  shmPtr = ptr;
  shmW = w;
  shmH = h;
}

export function onVideoInState(active: boolean): void {
  camera.guestActive = active;
  void syncStream();
}

// --- Header helpers ---------------------------------------------------------

function setConnected(v: boolean): void {
  const heap = getModuleHeap();
  if (!heap || !shmPtr) return;
  Atomics.store(heap.i32, shmPtr >> 2, v ? 1 : 0);
}

function resetSlots(): void {
  const heap = getModuleHeap();
  if (!heap || !shmPtr) return;
  Atomics.store(heap.i32, (shmPtr >> 2) + 1, -1); // active = none
}

// --- The frame pump ---------------------------------------------------------

// Copy one decoded frame into the non-active slot and flip. Cover-crops the
// camera image to the 4:3 target so arbitrary webcam aspect ratios fill the
// guest frame.
function pushFrame(ctx: CanvasRenderingContext2D, video: HTMLVideoElement): void {
  const heap = getModuleHeap();
  if (!heap || !shmPtr) return;
  const vw = video.videoWidth;
  const vh = video.videoHeight;
  if (!vw || !vh) return;
  const targetAspect = shmW / shmH;
  let sw = vw;
  let sh = vh;
  if (vw / vh > targetAspect) sw = Math.round(vh * targetAspect);
  else sh = Math.round(vw / targetAspect);
  const sx = (vw - sw) >> 1;
  const sy = (vh - sh) >> 1;
  ctx.drawImage(video, sx, sy, sw, sh, 0, 0, shmW, shmH);
  const img = ctx.getImageData(0, 0, shmW, shmH);

  const hdr = shmPtr >> 2;
  const activeIdx = Atomics.load(heap.i32, hdr + 1);
  const writeIdx = activeIdx === 0 ? 1 : 0; // never touch the active slot
  const slotPtr = shmPtr + HDR_BYTES + writeIdx * shmW * shmH * 4;
  heap.u8.set(img.data, slotPtr);
  Atomics.store(heap.i32, hdr + 1, writeIdx); // flip
  Atomics.add(heap.i32, hdr + 2, 1); // seq++
}

function startPump(video: HTMLVideoElement): void {
  if (!canvasEl) {
    canvasEl = document.createElement('canvas');
    canvasEl.width = shmW;
    canvasEl.height = shmH;
  }
  const ctx = canvasEl.getContext('2d', { willReadFrequently: true });
  if (!ctx) return;
  let stopped = false;
  // Prefer requestVideoFrameCallback (fires per camera frame); fall back to
  // requestAnimationFrame on browsers without it.
  type RVFCVideo = HTMLVideoElement & { requestVideoFrameCallback?: (cb: () => void) => number };
  const rvfc = (video as RVFCVideo).requestVideoFrameCallback?.bind(video);
  const step = (): void => {
    if (stopped) return;
    pushFrame(ctx, video);
    if (rvfc) rvfc(step);
    else requestAnimationFrame(step);
  };
  if (rvfc) rvfc(step);
  else requestAnimationFrame(step);
  pumpStop = () => {
    stopped = true;
  };
}

// --- Stream lifecycle -------------------------------------------------------

async function acquireStream(): Promise<MediaStream | null> {
  try {
    return await navigator.mediaDevices.getUserMedia({
      video: { width: { ideal: shmW }, height: { ideal: shmH } },
      audio: false,
    });
  } catch {
    return null;
  }
}

function stopStream(): void {
  pumpStop?.();
  pumpStop = null;
  if (stream) {
    for (const t of stream.getTracks()) t.stop();
    stream = null;
  }
  if (videoEl) {
    videoEl.srcObject = null;
    videoEl = null;
  }
  camera.live = false;
}

// Reconcile the physical camera with (enabled && guestActive).
async function syncStream(): Promise<void> {
  const want = camera.enabled && camera.guestActive;
  if (want && !stream) {
    const s = await acquireStream();
    // The toggle may have flipped while the permission prompt was up.
    if (!s) {
      if (camera.enabled) {
        showNotification('Camera unavailable — permission denied or no device', 'warning');
        await setCameraEnabled(false);
      }
      return;
    }
    if (!(camera.enabled && camera.guestActive)) {
      for (const t of s.getTracks()) t.stop();
      return;
    }
    stream = s;
    videoEl = document.createElement('video');
    videoEl.muted = true;
    videoEl.playsInline = true;
    videoEl.srcObject = s;
    try {
      await videoEl.play();
    } catch {
      /* autoplay of a muted camera element does not reject in practice */
    }
    startPump(videoEl);
    camera.live = true;
  } else if (!want && stream) {
    stopStream();
  }
}

// --- The master toggle ------------------------------------------------------

// User intent: connect/disconnect the camera from the guest's video input.
// Connecting selects the `host` source on machine.videoin (the DMSD then
// reports signal lock); the physical camera attaches only while the guest
// captures (syncStream).
export async function setCameraEnabled(on: boolean): Promise<void> {
  camera.enabled = on;
  if (on) {
    // `connected` goes true on the toggle, not on the first frame: the guest
    // reads it (as the DMSD's signal-lock bit) to decide whether to START
    // capturing, and frames only flow once capture is running — gating it on
    // frames would deadlock. Until the first frame lands the active slot is
    // -1, so the digitizer captures black rather than stale pixels.
    resetSlots();
    setConnected(true);
    await gsEval('machine.videoin.source', ['host']);
    // Prime the permission prompt under the click's user gesture, then
    // release the device again if the guest is not capturing yet.
    await syncStream();
    if (!camera.guestActive && camera.enabled) {
      const s = await acquireStream();
      if (s) {
        for (const t of s.getTracks()) t.stop();
      } else {
        showNotification('Camera unavailable — permission denied or no device', 'warning');
        await setCameraEnabled(false);
      }
    }
  } else {
    setConnected(false);
    stopStream();
    await gsEval('machine.videoin.source', ['none']);
  }
}

// Re-apply the user's camera intent after a (re)boot: machine.videoin
// resets to `none` with the machine, while `camera.enabled` is UI intent
// that survives it. Called from initEmulator once capabilities are known.
export async function reapplyCameraSource(): Promise<void> {
  if (!machine.videoIn) {
    // The new machine has no digitizer: drop the stream and the intent.
    if (camera.enabled) await setCameraEnabled(false);
    camera.guestActive = false;
    return;
  }
  if (camera.enabled) {
    setConnected(true);
    await gsEval('machine.videoin.source', ['host']);
  }
}
