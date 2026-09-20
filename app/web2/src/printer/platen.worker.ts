// The emulated LaserWriter's interpreter worker: runs EfterScript's platen
// library (its own non-threaded Emscripten module, build/platen-<version>.js)
// against the printer bridge's shared-memory ring
// (laserwriter_ring_protocol.h / platenProtocol.ts).  Why a worker of its
// own: the emulator is a threaded module and the library's Rust standard
// library is not, so it cannot link into the main module; and a job may
// interpret for seconds, which neither the page nor the emulation pthread
// can afford.  The page starts this worker on the first print job
// (printer/platen.ts), hands it the module URL, then the wasm memory and
// the control block's address; from then on everything but the finished
// PDF crosses through shared memory.  The loop itself is platenRing.ts.
//
// Messages in: start {moduleUrl, wasmUrl}; attach {memory, ctrl}.
// Messages out: ready; lost {reason}; document {jobId, title, name,
// pages, pdf} (the PDF transferred).

import { loadPlatenModule, PlatenLib } from './platenLib';
import { markRingLost, PlatenRing, type DocumentMsg } from './platenRing';

interface StartMsg {
  type: 'start';
  moduleUrl: string;
  wasmUrl: string;
}
interface AttachMsg {
  type: 'attach';
  memory: WebAssembly.Memory | SharedArrayBuffer;
  ctrl: number;
}
type InMsg = StartMsg | AttachMsg;

let lib: PlatenLib | null = null;
let ring: PlatenRing | null = null;
let lost = false;

// Gives up for good: the core fails its outstanding request and the page
// logs the reason.
function giveUp(reason: string): void {
  lost = true;
  ring?.markLost();
  postMessage({ type: 'lost', reason });
}

// Fetches and instantiates the module (the first print job waits for it,
// like a printer warming up).
async function start(msg: StartMsg): Promise<void> {
  try {
    lib = new PlatenLib(await loadPlatenModule(msg.moduleUrl, msg.wasmUrl));
    postMessage({ type: 'ready' });
  } catch (e) {
    console.error('[platen] module failed to load:', e);
    giveUp(`module failed to load: ${String(e)}`);
  }
}

// Attaches the loop to the bridge's control block and runs it.
function attach(msg: AttachMsg): void {
  if (lost) return;
  if (!lib) {
    giveUp('attach before the module loaded');
    return;
  }
  try {
    ring = new PlatenRing(msg.memory, msg.ctrl, lib, {
      post: (doc: DocumentMsg, transfer: Transferable[]) => postMessage(doc, { transfer }),
    });
  } catch (e) {
    console.error('[platen] attach failed:', e);
    const buffer = msg.memory instanceof SharedArrayBuffer ? msg.memory : msg.memory.buffer;
    markRingLost(buffer, msg.ctrl);
    giveUp(`attach failed: ${String(e)}`);
    return;
  }
  ring.run().catch((e: unknown) => {
    console.error('[platen] worker loop failed:', e);
    giveUp(`worker loop failed: ${String(e)}`);
  });
}

self.onmessage = (ev: MessageEvent<InMsg>) => {
  const msg = ev.data;
  if (msg.type === 'start') void start(msg);
  else if (msg.type === 'attach') attach(msg);
};
