// AudioWorklet tap for the AV microphone path (state/microphone.svelte.ts).
//
// Served as a real file rather than a blob: URL: addModule() on a blob is
// rejected under the cross-origin isolation the emulator needs for
// SharedArrayBuffer, and the failure is silent — it falls back to the
// deprecated ScriptProcessorNode, or to nothing at all.
class GsMicTap extends AudioWorkletProcessor {
  process(inputs) {
    const ch = inputs[0] && inputs[0][0];
    if (ch && ch.length) this.port.postMessage(ch.slice(0));
    return true; // outputs stay silent; this node exists only to tap
  }
}
registerProcessor('gs-mic-tap', GsMicTap);
