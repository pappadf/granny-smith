// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// em_audio.c
// Audio subsystem for Emscripten platform — implements WebAudio streaming via
// AudioWorklet with a ring buffer. One parameterized stream serves every
// machine: interleaved int16 frames, mono or stereo, at a runtime-settable
// source rate (22,255 / 22,257 / 22,050 / 44,100 Hz).
//
// Real-time machinery lives here on the consumer side (per the sound-emulation
// strategy): linear-interpolation resampling with PI rate trim (±2000 ppm),
// a start gate at the target depth, silence-aware depth trimming, per-quantum
// volume ramping, and a one-pole LPF. Producers stay deterministic.
//
// With PROXY_TO_PTHREAD, the emulator runs on a worker thread.  AudioContext
// and AudioWorklet are main-thread-only APIs, so all WebAudio calls must be
// proxied to the main thread.  We define EM_JS functions for the JS code
// and use emscripten_sync_run_in_main_runtime_thread() to ensure they
// execute on the main thread.

// ============================================================================
// Includes
// ============================================================================

#include "em.h"

#include <emscripten.h>
#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"

// ============================================================================
// WebAudio (AudioWorklet) Implementation
// ============================================================================

// Initialize the WebAudio subsystem: create the context and register the
// worklet processor. The worklet *node* is created lazily by gs_audio_open_js
// once the machine's sound frontend declares its stream parameters.
// clang-format off
EM_JS(void, gs_audio_init_js, (), {
    // Check if already initialized
    if (Module.gsAudio && Module.gsAudio.initialized)
        return;

    // Initialize audio subsystem object
    var ga = Module.gsAudio = (Module.gsAudio || {});
    ga.initialized = true;
    ga.targetLatency = 0.083; // fixed latency target (~5 Plus VBLs)
    ga.srcRate = 0;           // set by gs_audio_open_js
    ga.channels = 0;
    ga.node = null;
    ga.pend = null;           // stream params requested before the module loaded
    ga.modReady = false;
    ga.ctx = ga.ctx || new (self.AudioContext || self.webkitAudioContext)();

    // AudioWorklet processor — internal int16 frame ring fed via MessagePort.
    var RING_FRAMES = 1 << 16; // 65536 frames ~ 2.9s at 22 kHz
    var workletCode = [
        "class GSAudioProcessor extends AudioWorkletProcessor {",
        "  constructor(opts){",
        "    super();",
        "    var o=opts.processorOptions;",
        "    this.ch=o.channels||1;",
        "    this.srcRate=o.srcRate||22257;",
        "    this.targetLatency=o.targetLatency||0.083;",
        "    var RF=" + RING_FRAMES + ";",
        "    this.mask=RF-1;",
        "    this.data=new Int16Array(RF*this.ch);",
        "    this.wIdx=0; this.rIdx=0;", // ring indices in FRAMES
        "    this.vol=0; this.silentCount=0; this.underruns=0;",
        "    this.targetFrames=Math.floor(this.targetLatency*this.srcRate);",
        "    this.dstRate=sampleRate;",
        "    this.step=this.srcRate/this.dstRate;",
        "    this.errI=0; this.frac=0; this.lpfY=[0,0];",
        "    var fc=8000; this.lpfA=1.0-Math.exp(-2*Math.PI*fc/this.dstRate);",
        // DC blocker (speaker AC coupling): the ASC DAC output carries a large
        // constant offset (offset-binary DAC, unused wavetable voices at rail),
        // and the underrun/start-gate fill value is 0 — without DC removal
        // every delivery hiccup steps between the offset and 0, an audible
        // full-scale click. fc ~20 Hz: inaudible, settles in a few ms.
        "    this.dcX=[0,0]; this.dcYv=[0,0];",
        "    this.dcR=1-2*Math.PI*20/this.dstRate;",
        "    this.quietQ=0;", // process() quanta since the last data push
        "    this.curGain=0; this.started=false;",
        "    var self2=this;",
        "    this.port.onmessage=function(e){",
        "      var d=e.data;",
        "      if(d.rate){", // rate change = stream restart: flush, re-gate, ramp
        "        self2.srcRate=d.rate;",
        "        self2.step=self2.srcRate/self2.dstRate;",
        "        self2.targetFrames=Math.floor(self2.targetLatency*self2.srcRate);",
        "        self2.rIdx=self2.wIdx; self2.frac=0; self2.errI=0;",
        "        self2.started=false; self2.curGain=0;",
        "        return;",
        "      }",
        "      var src=new Int16Array(d.buf);",
        "      var ch=self2.ch;",
        "      var n=(src.length/ch)|0;", // payload length in frames
        "      self2.vol=d.vol;",
        "      self2.quietQ=0;",
        "      if(d.sil){self2.silentCount++;}else{self2.silentCount=0;}",
        "      var w=self2.wIdx, r=self2.rIdx, mask=self2.mask;",
        "      var used=(w-r)&mask, free=mask-used;",
        "      if(n>free){self2.rIdx=(r+n-free)&mask;}", // overwrite oldest
        "      for(var i=0;i<n;i++){",
        "        var di=((w+i)&mask)*ch;",
        "        for(var c=0;c<ch;c++)self2.data[di+c]=src[i*ch+c];",
        "      }",
        "      self2.wIdx=(w+n)&mask;",
        "    };",
        "  }",
        "  process(inputs,outputs){",
        "    var out=outputs[0]; var frames=out[0].length;",
        "    var ch=this.ch; var oc=out.length<ch?out.length:ch;",
        "    var w=this.wIdx, r=this.rIdx, mask=this.mask;",
        "    var avail=(w-r)&mask;",
        "    this.quietQ++;",
        // Start gate: stay silent until the ring reaches the target depth, OR
        // until the stream has evidently ended short of it (data pending but
        // no push for 12 quanta ~32 ms) — so sounds shorter than the cushion
        // still play out. An active producer pushes every ~3 ms, so the quiet
        // threshold must sit above main-thread jank (tens of ms) or the gate
        // opens mid-fill with a shallow ring and the crackle returns.
        "    if(!this.started){",
        "      var ready=avail>=this.targetFrames||(avail>0&&this.quietQ>=12);",
        "      if(!ready){for(var c=0;c<out.length;c++)out[c].fill(0);return true;}",
        "      this.started=true;",
        "    }",
        "    var ts=this.targetFrames;",
        "    var targetGain=Math.min(Math.max(this.vol,0),7)/7;",
        // PI controller trims the resample step ±2000 ppm toward target depth
        "    var error=avail-ts;",
        "    this.errI=this.errI*0.995+error*0.005;",
        "    var adj=(error/ts)*0.005+(this.errI/ts)*0.001;",
        "    if(adj>0.002)adj=0.002;else if(adj<-0.002)adj=-0.002;",
        "    var stepAdj=this.step*(1+adj);",
        "    var gainStep=(targetGain-this.curGain)/frames;",
        "    for(var i=0;i<frames;i++){",
        "      var have=(w-r)&mask;",
        "      if(have<2){this.underruns++;this.curGain+=gainStep;for(var c=0;c<oc;c++)out[c][i]=0;continue;}",
        "      var i0=this.frac|0; var i1=i0+1; if(i1>=have)i1=have-1;",
        "      var b0=((r+i0)&mask)*ch, b1=((r+i1)&mask)*ch;",
        "      var frac=this.frac-i0;",
        "      this.curGain+=gainStep;",
        "      for(var c=0;c<oc;c++){",
        "        var s0=this.data[b0+c]/32768, s1=this.data[b1+c]/32768;",
        "        var x=s0+(s1-s0)*frac;", // linear interpolation
        "        this.lpfY[c]+=this.lpfA*(x-this.lpfY[c]);", // one-pole LPF
        "        var lp=this.lpfY[c];",
        "        var hp=lp-this.dcX[c]+this.dcR*this.dcYv[c];", // DC blocker
        "        this.dcX[c]=lp; this.dcYv[c]=hp;",
        "        out[c][i]=hp*this.curGain;",
        "      }",
        "      this.frac+=stepAdj;",
        "      var consumed=this.frac|0;",
        "      if(consumed>0){this.frac-=consumed;var wc=consumed<have?consumed:have;r=(r+wc)&mask;}",
        "    }",
        "    this.rIdx=r;",
        // Re-arm the start gate when the stream runs dry (a sound ended, or a
        // deep underrun): without this only the FIRST sound after page load
        // gets the target-depth cushion — every later one plays against a
        // near-empty ring and crackles on every scheduling hiccup. The <2
        // remnant frame must be discarded (rIdx=wIdx): a nonzero depth would
        // let the quiet-stream early-open defeat the gate for the next sound.
        "    if(this.started&&((this.wIdx-r)&mask)<2){",
        "      this.started=false;this.rIdx=this.wIdx;this.frac=0;this.errI=0;",
        "    }",
        // Silence-aware depth trim: during sustained silence, clamp ring depth
        // to the target so latency can't grow when the emulator outruns real time
        "    if(this.silentCount>=8){",
        "      var depth=(this.wIdx-this.rIdx)&mask;",
        "      if(depth>ts){this.rIdx=(this.rIdx+depth-ts)&mask;}",
        "    }",
        "    return true;",
        "  }",
        "}",
        "registerProcessor('gs-audio-worklet',GSAudioProcessor);"
    ].join("\n");

    // (Re)creates the worklet node for the pending stream parameters
    ga.makeNode = function() {
        var p = ga.pend;
        if (!p)
            return;
        ga.pend = null;
        if (ga.node) {
            try {
                ga.node.disconnect();
            } catch (e) {
            }
            ga.node = null;
        }
        ga.srcRate = p.rate;
        ga.channels = p.ch;
        try {
            ga.node = new AudioWorkletNode(ga.ctx, 'gs-audio-worklet', {
                numberOfInputs: 0,
                numberOfOutputs: 1,
                outputChannelCount: [p.ch],
                processorOptions: {
                    srcRate: p.rate,
                    channels: p.ch,
                    targetLatency: ga.targetLatency
                }
            });
            ga.node.connect(ga.ctx.destination);
            console.log('[audio] worklet open rate=' + p.rate + 'Hz ch=' + p.ch +
                        ' targetLatency=' + (ga.targetLatency * 1000).toFixed(1) + 'ms');
        } catch (e) {
            console.error('[audio] worklet node creation failed', e);
        }
    };

    // Create worklet module from blob URL, then honor any pending open
    var blobURL = URL.createObjectURL(new Blob([workletCode], {type: 'application/javascript'}));
    ga.ctx.audioWorklet.addModule(blobURL)
        .then(function() {
            try {
                URL.revokeObjectURL(blobURL);
            } catch (e) {
            }
            ga.modReady = true;
            ga.makeNode();
        })
        .catch(function(e) { console.error('[audio] addModule failed', e); });
});

// Resume audio context if suspended
EM_JS(void, gs_audio_resume_js, (), {
    try {
        const ga = Module.gsAudio;
        if (ga && ga.ctx && ga.ctx.state === 'suspended')
            ga.ctx.resume();
    } catch (e) {
    }
});

// Open (or re-parameterize) the stream: same channel count = in-place rate
// change (worklet restart), different channel count = node re-creation.
EM_JS(void, gs_audio_open_js, (int rate, int channels), {
    gs_audio_init_js();
    var ga = Module.gsAudio;
    if (ga.node && ga.channels === channels) {
        if (ga.srcRate !== rate) {
            ga.srcRate = rate;
            ga.node.port.postMessage({rate: rate});
        }
        return;
    }
    ga.pend = {rate: rate, ch: channels};
    if (ga.modReady)
        ga.makeNode();
});

// Change the source sample rate mid-stream (stream restart in the worklet)
EM_JS(void, gs_audio_set_rate_js, (int rate), {
    var ga = Module.gsAudio;
    if (!ga)
        return;
    if (ga.pend) {
        ga.pend.rate = rate;
        return;
    }
    if (ga.srcRate === rate)
        return;
    ga.srcRate = rate;
    if (ga.node)
        ga.node.port.postMessage({rate: rate});
});

// Push interleaved int16 frames to the worklet via MessagePort
EM_JS(void, gs_audio_push_js, (uint32_t ptr, int nframes, int volume), {
    var ga = Module.gsAudio;
    if (!ga || !ga.ctx || !ga.node || nframes <= 0)
        return;
    gs_audio_resume_js();

    var heap = (typeof HEAPU8 !== 'undefined') ? HEAPU8 : Module.HEAPU8;
    if (!heap)
        return;
    var bytes = nframes * ga.channels * 2;
    var end = Math.min(ptr + bytes, heap.length);
    if (ptr >= end || ((end - ptr) & 1))
        return;

    // Copy samples from WASM heap (must copy — heap can grow/move); the
    // slice's fresh ArrayBuffer is viewed as Int16Array in the worklet.
    var raw = heap.slice(ptr, end);
    var samples = new Int16Array(raw.buffer);

    // Silence detection (all samples equal — any DC level counts)
    var silent = true;
    for (var k = 1; k < samples.length; k++) {
        if (samples[k] !== samples[0]) {
            silent = false;
            break;
        }
    }

    // Send to worklet via MessagePort (transfer the underlying buffer)
    ga.node.port.postMessage(
        {buf: raw.buffer, vol: (volume | 0) & 7, sil: silent},
        [raw.buffer]
    );
});
// clang-format on

// ============================================================================
// Main-thread Proxy Wrappers
// ============================================================================

// These wrappers ensure audio EM_JS functions execute on the main thread
// (where AudioContext and AudioWorklet are available), even though the
// emulator runs on a worker thread via PROXY_TO_PTHREAD.

static void gs_audio_init(void) {
    if (emscripten_is_main_browser_thread()) {
        gs_audio_init_js();
    } else {
        emscripten_sync_run_in_main_runtime_thread(EM_FUNC_SIG_V, gs_audio_init_js);
    }
}

static void gs_audio_resume(void) {
    if (emscripten_is_main_browser_thread()) {
        gs_audio_resume_js();
    } else {
        emscripten_sync_run_in_main_runtime_thread(EM_FUNC_SIG_V, gs_audio_resume_js);
    }
}

static void gs_audio_open(int rate, int channels) {
    if (emscripten_is_main_browser_thread()) {
        gs_audio_open_js(rate, channels);
    } else {
        emscripten_sync_run_in_main_runtime_thread(EM_FUNC_SIG_VII, gs_audio_open_js, rate, channels);
    }
}

static void gs_audio_set_rate(int rate) {
    if (emscripten_is_main_browser_thread()) {
        gs_audio_set_rate_js(rate);
    } else {
        emscripten_sync_run_in_main_runtime_thread(EM_FUNC_SIG_VI, gs_audio_set_rate_js, rate);
    }
}

static void gs_audio_push(uint32_t ptr, int nframes, int volume) {
    if (emscripten_is_main_browser_thread()) {
        gs_audio_push_js(ptr, nframes, volume);
    } else {
        emscripten_sync_run_in_main_runtime_thread(EM_FUNC_SIG_VIII, gs_audio_push_js, ptr, nframes, volume);
    }
}

// ============================================================================
// Operations (Public API)
// ============================================================================

// Initialize audio subsystem (context + worklet module; node comes with open)
void em_audio_init(void) {
    gs_audio_init();
}

// Resume audio context
void em_audio_resume(void) {
    gs_audio_resume();
}

// ============================================================================
// Platform Interface Implementation
// ============================================================================

// Open (or re-parameterize) the host audio stream
void platform_audio_open(uint32_t src_rate_hz, int channels) {
    gs_audio_open((int)src_rate_hz, channels);
}

// Push interleaved int16 frames to the stream
void platform_audio_push(const int16_t *frames, int nframes, int vol_0_7) {
    if (!frames || nframes <= 0)
        return;
    gs_audio_push((uint32_t)(uintptr_t)frames, nframes, vol_0_7);
}

// Change the source sample rate mid-stream
void platform_audio_set_rate(uint32_t src_rate_hz) {
    gs_audio_set_rate((int)src_rate_hz);
}
