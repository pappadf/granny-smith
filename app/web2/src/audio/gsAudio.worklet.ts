// The audio-out AudioWorklet processor: a thin shell around AudioRingConsumer
// (audioRing.ts), which reads int16 frames straight from the emulator's ring
// in the wasm heap. Bundled by Vite and handed to the core as
// Module.gsAudioWorkletUrl (bus/emulator.ts); em_audio.c creates the node.

import { AudioRingConsumer } from './audioRing';

// The AudioWorkletGlobalScope, which the DOM typings do not describe.
declare const sampleRate: number;
declare class AudioWorkletProcessor {
  readonly port: MessagePort;
  constructor(options?: unknown);
}
declare function registerProcessor(name: string, ctor: unknown): void;

interface ProcessorOptions {
  sab: SharedArrayBuffer;
  ringPtr: number;
  srcRate: number;
  channels: number;
  targetLatency: number;
  owner: number;
}

class GSAudioProcessor extends AudioWorkletProcessor {
  private ring: AudioRingConsumer | null = null;

  constructor(opts: { processorOptions: ProcessorOptions }) {
    super();
    const o = opts.processorOptions;
    try {
      this.ring = new AudioRingConsumer(o.sab, o.ringPtr, { ...o, dstRate: sampleRate });
    } catch (e) {
      console.error('[audio] worklet cannot use the ring:', e);
    }
    this.port.onmessage = (e: MessageEvent<{ stop?: number; rate?: number }>) => {
      if (e.data.stop) this.ring?.retire();
      else if (e.data.rate) this.ring?.setRate(e.data.rate);
    };
  }

  process(_inputs: Float32Array[][], outputs: Float32Array[][]): boolean {
    return this.ring ? this.ring.process(outputs[0]) : false;
  }
}

registerProcessor('gs-audio-worklet', GSAudioProcessor);
