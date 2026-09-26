// Camera state — the browser side of the AV video-in path
// (proposal-av-video-in.md §2.3; C side: src/platform/wasm/em_camera.c).
//
// Transport: em_camera.c owns a static double-buffered frame slot pair
// behind a control block (bus/shmLayout.ts) in the shared wasm heap and
// announces its address once via Module.onVideoInReady. This module reads
// the layout from the block, writes each decoded webcam frame into the
// NON-active slot through the live heap views, flips `active`, then bumps
// `seq`; the emulator worker copies out of the active slot at field cadence
// and retries a copy a completed frame overlapped (a seqlock on `seq` -- the
// slot flip alone does not rule a tear out). Staleness is at most one frame.
//
// Lifecycle & privacy: the camera light is on only while the guest actually
// captures. `enabled` is the user's master toggle (the click is also the
// user gesture getUserMedia wants); `guestActive` follows the VDC clock via
// Module.onVideoInState. The MediaStreamTrack is attached only while BOTH
// hold; a permission-priming getUserMedia runs at enable time and is
// stopped immediately when the guest is not capturing.

import { gsEval, getModuleHeap } from '@/bus/emulator';
import {
  CAM_MAGIC,
  CAM_VERSION,
  CAM_W_ACTIVE,
  CAM_W_CONNECTED,
  CAM_W_HEIGHT,
  CAM_W_SEQ,
  CAM_W_SLOT_BYTES,
  CAM_W_SLOT_OFF,
  CAM_W_WIDTH,
  shmBlockOk,
} from '@/bus/shmLayout';
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

// Shared-heap transport, announced by em_camera.c at startup; the layout
// comes from its control block.
let shmPtr = 0;
let shmW = 640;
let shmH = 480;
let slotOff = 0;
let slotBytes = 0;

let stream: MediaStream | null = null;
let videoEl: HTMLVideoElement | null = null;
let canvasEl: HTMLCanvasElement | null = null;
let pumpStop: (() => void) | null = null;

// --- Module callbacks (attached in bus/emulator.ts) ------------------------

// The block's address arrives while the module is still starting, so it is
// read (and checked) on first use.
let announcedPtr = 0;
let layoutChecked = false;

export function onVideoInReady(ptr: number): void {
  announcedPtr = ptr;
  layoutChecked = false;
  shmPtr = 0;
}

// The transport's heap views, once its control block has been read and
// found to be the layout this build speaks; null otherwise.
function transport(): ReturnType<typeof getModuleHeap> {
  const heap = getModuleHeap();
  if (!heap || !announcedPtr) return null;
  if (!layoutChecked) {
    layoutChecked = true;
    if (!shmBlockOk(heap.i32, announcedPtr, CAM_MAGIC, CAM_VERSION)) {
      // A core from another build: writing through a guessed layout would
      // corrupt the heap. Say so, and leave the camera off.
      console.error('[camera] frame transport layout not recognised; camera disabled');
      showNotification(
        'Camera unavailable: this emulator build has a different frame layout',
        'error',
      );
      return null;
    }
    const w = announcedPtr >> 2;
    slotOff = Atomics.load(heap.i32, w + CAM_W_SLOT_OFF);
    slotBytes = Atomics.load(heap.i32, w + CAM_W_SLOT_BYTES);
    shmW = Atomics.load(heap.i32, w + CAM_W_WIDTH);
    shmH = Atomics.load(heap.i32, w + CAM_W_HEIGHT);
    shmPtr = announcedPtr;
  }
  return shmPtr ? heap : null;
}

export function onVideoInState(active: boolean): void {
  camera.guestActive = active;
  void syncStream();
}

// --- Header helpers ---------------------------------------------------------

function setConnected(v: boolean): void {
  const heap = transport();
  if (!heap) return;
  Atomics.store(heap.i32, (shmPtr >> 2) + CAM_W_CONNECTED, v ? 1 : 0);
}

function resetSlots(): void {
  const heap = transport();
  if (!heap) return;
  Atomics.store(heap.i32, (shmPtr >> 2) + CAM_W_ACTIVE, -1); // active = none
}

// --- The frame pump ---------------------------------------------------------

// Copy one decoded frame into the non-active slot and flip. Cover-crops the
// camera image to the 4:3 target so arbitrary webcam aspect ratios fill the
// guest frame.
function pushFrame(ctx: CanvasRenderingContext2D, video: HTMLVideoElement): void {
  const heap = transport();
  if (!heap) return;
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
  const activeIdx = Atomics.load(heap.i32, hdr + CAM_W_ACTIVE);
  const writeIdx = activeIdx === 0 ? 1 : 0; // never touch the active slot
  const slotPtr = shmPtr + slotOff + writeIdx * slotBytes;
  heap.u8.set(img.data, slotPtr);
  Atomics.store(heap.i32, hdr + CAM_W_ACTIVE, writeIdx); // flip
  Atomics.add(heap.i32, hdr + CAM_W_SEQ, 1); // seq++: the reader's seqlock
}

function startPump(video: HTMLVideoElement): void {
  if (!transport()) return; // the frame geometry below comes from the block
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
//
// Serialised, as the microphone's is: the guest's capture gate and the
// user's toggle can fire together, and two overlapping runs each saw no
// stream across the getUserMedia await, each acquired one, and each started
// a pump -- the first camera was never stopped, its light stayed on, and two
// pumps wrote the slots (F-41).
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
    const video = document.createElement('video');
    videoEl = video;
    video.muted = true;
    video.playsInline = true;
    video.srcObject = s;
    try {
      await video.play();
    } catch {
      /* autoplay of a muted camera element does not reject in practice */
    }
    // The toggle may have gone off during play(): stopStream() released this
    // stream, and there is nothing to pump (N-59: the module's videoEl was
    // read here, null by then).
    if (stream !== s) return;
    startPump(video);
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
