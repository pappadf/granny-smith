// AudioWorklet tap for the AV microphone path (state/microphone.svelte.ts).
//
// Served as a real file rather than a blob: URL: addModule() on a blob is
// rejected under the cross-origin isolation the emulator needs for
// SharedArrayBuffer, and the failure is silent — it falls back to the
// deprecated ScriptProcessorNode, or to nothing at all.
class GsMicTap extends AudioWorkletProcessor {
  process(inputs) {
    const chans = inputs[0];
    if (!chans || !chans.length) return true;
    const n = chans[0].length;
    if (!n) return true;

    // MIX DOWN every channel — do not just take chans[0].  getUserMedia
    // treats `channelCount: 1` as a preference, not a requirement, so a
    // device is free to hand back a stereo track (Chromium's own fake device
    // does).  Plenty of real interfaces then carry the microphone on the
    // right channel alone and leave the left at digital zero, which taking
    // channel 0 turns into a perfectly healthy stream of silence: the ring
    // fills at exactly the right rate, nothing underruns, and the guest
    // records nothing.  Averaging costs one pass and cannot produce that.
    let mixed;
    if (chans.length === 1) {
      mixed = chans[0].slice(0);
    } else {
      mixed = new Float32Array(n);
      for (let c = 0; c < chans.length; c++) {
        const src = chans[c];
        for (let i = 0; i < n; i++) mixed[i] += src[i];
      }
      const inv = 1 / chans.length;
      for (let i = 0; i < n; i++) mixed[i] *= inv;
    }
    this.port.postMessage(mixed);
    return true; // outputs stay silent; this node exists only to tap
  }
}
registerProcessor('gs-mic-tap', GsMicTap);
