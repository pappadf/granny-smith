// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// em_audio.c
// Audio subsystem for Emscripten platform — implements WebAudio streaming via
// AudioWorklet fed from a SharedArrayBuffer ring (perf proposal P6). One
// parameterized stream serves every machine: interleaved int16 frames, mono
// or stereo, at a runtime-settable source rate (22,255 / 22,257 / 22,050 /
// 44,100 Hz).
//
// Real-time machinery lives here on the consumer side (per the sound-emulation
// strategy): linear-interpolation resampling with PI rate trim (±2000 ppm),
// a start gate at the target depth, silence-aware depth trimming, per-quantum
// volume ramping, and a one-pole LPF. Producers stay deterministic.
//
// With PROXY_TO_PTHREAD, the emulator runs on a worker thread.  AudioContext
// and AudioWorklet are main-thread-only APIs, so the *control-plane* calls
// (init / open / rate change / resume) are proxied to the main thread with
// emscripten_sync_run_in_main_runtime_thread().  The *data plane* is not:
// with -pthread the wasm heap is a SharedArrayBuffer whose static addresses
// never move (shared WebAssembly.Memory grows in place, it never replaces
// the buffer), so the worklet consumes frames directly from a C-side ring
// via Atomics.  platform_audio_push used to be a synchronous worker→main
// round trip (~348/s during playback) doing a heap copy, a silence scan and
// a transfer postMessage — recurring stalls of the emulation thread whose
// jitter fed the governor's audio back-off.  It is now a local memcpy plus
// two atomic stores, and the governor's ring-fill query is an atomic load.

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

#include <stdatomic.h>

// ============================================================================
// SharedArrayBuffer audio ring (emulator worker → AudioWorklet)
// ============================================================================

// Ring geometry: power-of-two frame count, ~2.9 s at 22 kHz.  The struct
// lives in static storage, so its address is fixed for the process lifetime
// and stays valid across wasm memory growth (shared memory grows in place).
#define GS_ARING_FRAMES 65536
#define GS_ARING_MAX_CH 2

// Header field indices, as seen by the worklet's Int32Array view.  Keep in
// sync with the worklet code below.
//   [0] write_idx      frames, masked; producer-owned (release-stored last)
//   [1] read_idx       frames, masked; consumer-owned (producer CAS-advances
//                      it only on overflow, overwrite-oldest semantics)
//   [2] vol            0..7, latest producer volume
//   [3] silent_pushes  consecutive all-equal pushes (silence-aware depth trim)
//   [4] fill_pm        worklet→producer: ring fill vs target depth, per-mille
//   [5] push_seq       bumped per push; the worklet's quiet-stream detector
typedef struct gs_audio_ring {
    _Atomic uint32_t write_idx;
    _Atomic uint32_t read_idx;
    _Atomic int32_t vol;
    _Atomic int32_t silent_pushes;
    _Atomic int32_t fill_pm;
    _Atomic uint32_t push_seq;
    _Atomic uint32_t reserved[2]; // pad header to 32 bytes
    int16_t data[GS_ARING_FRAMES * GS_ARING_MAX_CH];
} gs_audio_ring_t;

static gs_audio_ring_t g_aring; // the one shared ring
static int g_aring_channels = 1; // set by platform_audio_open
static double g_last_push_time = -1.0; // producer-side freshness for ring_fill

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

    // Autoplay policy: a suspended AudioContext may only resume from a user
    // gesture.  The data path no longer touches the main thread (SAB ring),
    // so the old per-push resume nudge is gone — hook input gestures once
    // instead (resume() on a running context is a no-op).
    var resumeOnGesture = function() {
        try {
            if (ga.ctx.state === 'suspended')
                ga.ctx.resume();
        } catch (e) {
        }
    };
    self.addEventListener('pointerdown', resumeOnGesture, true);
    self.addEventListener('keydown', resumeOnGesture, true);

    // AudioWorklet processor — consumes int16 frames straight from the
    // emulator's SharedArrayBuffer ring (the wasm heap): write/read indices,
    // volume, silence count and the fill report all live in the ring header,
    // accessed with Atomics. No per-push messages; the MessagePort carries
    // only rare control traffic (rate changes).
    var workletCode = [
        "class GSAudioProcessor extends AudioWorkletProcessor {",
        "  constructor(opts){",
        "    super();",
        "    var o=opts.processorOptions;",
        "    this.ch=o.channels||1;",
        "    this.srcRate=o.srcRate||22257;",
        "    this.targetLatency=o.targetLatency||0.083;",
        "    this.mask=(o.ringFrames||65536)-1;",
        // Header (Int32) + frame data (Int16) views over the wasm heap SAB.
        // The ring is static C storage: its address never changes, and shared
        // WebAssembly.Memory grows in place, so the views stay valid.
        "    this.hdr=new Int32Array(o.sab,o.ringPtr,8);",
        "    this.data=new Int16Array(o.sab,o.ringPtr+32,(this.mask+1)*this.ch);",
        "    this.underruns=0;",
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
        "    this.quietQ=0; this.lastSeq=0;", // quanta since the push_seq moved
        "    this.curGain=0; this.started=false;",
        "    var self2=this;",
        "    this.port.onmessage=function(e){",
        "      var d=e.data;",
        "      if(d.rate){", // rate change = stream restart: flush, re-gate, ramp
        "        self2.srcRate=d.rate;",
        "        self2.step=self2.srcRate/self2.dstRate;",
        "        self2.targetFrames=Math.floor(self2.targetLatency*self2.srcRate);",
        "        Atomics.store(self2.hdr,1,Atomics.load(self2.hdr,0));", // rIdx=wIdx
        "        self2.frac=0; self2.errI=0;",
        "        self2.started=false; self2.curGain=0;",
        "      }",
        "    };",
        "  }",
        "  process(inputs,outputs){",
        "    var out=outputs[0]; var frames=out[0].length;",
        "    var ch=this.ch; var oc=out.length<ch?out.length:ch;",
        "    var hdr=this.hdr, mask=this.mask;",
        "    var w=Atomics.load(hdr,0), r=Atomics.load(hdr,1);",
        "    var rStart=r;",
        "    var avail=(w-r)&mask;",
        // Quiet-stream detector: quanta since the producer's push_seq moved.
        "    var seq=Atomics.load(hdr,5);",
        "    if(seq!==this.lastSeq){this.lastSeq=seq;this.quietQ=0;}else{this.quietQ++;}",
        // Fill-level report every 32 quanta (~85 ms at 48 kHz): stored in the
        // ring header, read by the emulator's adaptive governor as back-
        // pressure (a draining ring = the real-time deadline being missed).
        "    if(((this.statQ=(this.statQ||0)+1)&31)===0){",
        "      Atomics.store(hdr,4,Math.max(0,Math.min(2000,Math.round(avail/(this.targetFrames||1)*1000))));",
        "    }",
        // Start gate: stay silent until the ring reaches the target depth, OR
        // until the stream has evidently ended short of it (data pending but
        // no push for 12 quanta ~32 ms) — so sounds shorter than the cushion
        // still play out. An active producer pushes every ~3 ms, so the quiet
        // threshold must sit above scheduling jank (tens of ms) or the gate
        // opens mid-fill with a shallow ring and the crackle returns.
        "    if(!this.started){",
        "      var ready=avail>=this.targetFrames||(avail>0&&this.quietQ>=12);",
        "      if(!ready){for(var c=0;c<out.length;c++)out[c].fill(0);return true;}",
        "      this.started=true;",
        "    }",
        "    var ts=this.targetFrames;",
        "    var targetGain=Math.min(Math.max(Atomics.load(hdr,2),0),7)/7;",
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
        // Publish consumption with CAS from the value we started from: if the
        // producer overflow-advanced read_idx meanwhile (overwrite-oldest),
        // its newer value wins and we resync next quantum — glitch-level
        // consequences only in an already-overflowing stream.
        "    Atomics.compareExchange(hdr,1,rStart,r);",
        // Re-arm the start gate when the stream runs dry (a sound ended, or a
        // deep underrun): without this only the FIRST sound after page load
        // gets the target-depth cushion — every later one plays against a
        // near-empty ring and crackles on every scheduling hiccup. The <2
        // remnant frame must be discarded (rIdx=wIdx): a nonzero depth would
        // let the quiet-stream early-open defeat the gate for the next sound.
        "    var w2=Atomics.load(hdr,0);",
        "    if(this.started&&((w2-r)&mask)<2){",
        "      this.started=false;Atomics.store(hdr,1,w2);this.frac=0;this.errI=0;",
        "    }",
        // Silence-aware depth trim: during sustained silence, clamp ring depth
        // to the target so latency can't grow when the emulator outruns real time
        "    if(Atomics.load(hdr,3)>=8){",
        "      var depth=(w2-Atomics.load(hdr,1))&mask;",
        "      if(depth>ts){Atomics.store(hdr,1,(Atomics.load(hdr,1)+depth-ts)&mask);}",
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
            // Hand the worklet the wasm heap (a SharedArrayBuffer under
            // -pthread) plus the ring's fixed address; it consumes frames
            // and publishes fill reports through it with Atomics.
            var heap = (typeof HEAPU8 !== 'undefined') ? HEAPU8 : Module.HEAPU8;
            ga.node = new AudioWorkletNode(ga.ctx, 'gs-audio-worklet', {
                numberOfInputs: 0,
                numberOfOutputs: 1,
                outputChannelCount: [p.ch],
                processorOptions: {
                    srcRate: p.rate,
                    channels: p.ch,
                    targetLatency: ga.targetLatency,
                    sab: heap.buffer,
                    ringPtr: p.ringPtr,
                    ringFrames: p.ringFrames
                }
            });
            ga.node.connect(ga.ctx.destination);
            // Opportunistic resume: covers a context suspended before the
            // node existed when a qualifying gesture already happened.
            try {
                if (ga.ctx.state === 'suspended')
                    ga.ctx.resume();
            } catch (e) {
            }
            console.log('[audio] worklet open rate=' + p.rate + 'Hz ch=' + p.ch +
                        ' targetLatency=' + (ga.targetLatency * 1000).toFixed(1) + 'ms (SAB ring)');
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
// ring_ptr / ring_frames locate the shared ring inside the wasm heap.
EM_JS(void, gs_audio_open_js, (int rate, int channels, uint32_t ring_ptr, int ring_frames), {
    gs_audio_init_js();
    var ga = Module.gsAudio;
    if (ga.node && ga.channels === channels) {
        if (ga.srcRate !== rate) {
            ga.srcRate = rate;
            ga.node.port.postMessage({rate: rate});
        }
        return;
    }
    ga.pend = {rate: rate, ch: channels, ringPtr: ring_ptr, ringFrames: ring_frames};
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
    uint32_t ring_ptr = (uint32_t)(uintptr_t)&g_aring;
    if (emscripten_is_main_browser_thread()) {
        gs_audio_open_js(rate, channels, ring_ptr, GS_ARING_FRAMES);
    } else {
        emscripten_sync_run_in_main_runtime_thread(EM_FUNC_SIG_VIIII, gs_audio_open_js, rate, channels, ring_ptr,
                                                   GS_ARING_FRAMES);
    }
}

static void gs_audio_set_rate(int rate) {
    if (emscripten_is_main_browser_thread()) {
        gs_audio_set_rate_js(rate);
    } else {
        emscripten_sync_run_in_main_runtime_thread(EM_FUNC_SIG_VI, gs_audio_set_rate_js, rate);
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
    if (channels < 1)
        channels = 1;
    if (channels > GS_ARING_MAX_CH)
        channels = GS_ARING_MAX_CH;
    g_aring_channels = channels;
    // Fresh stream: reset the ring so stale frames from a previous machine
    // or stream shape can't play into the new one.
    atomic_store_explicit(&g_aring.read_idx, atomic_load_explicit(&g_aring.write_idx, memory_order_relaxed),
                          memory_order_relaxed);
    atomic_store_explicit(&g_aring.silent_pushes, 0, memory_order_relaxed);
    atomic_store_explicit(&g_aring.fill_pm, -1, memory_order_relaxed);
    gs_audio_open((int)src_rate_hz, channels);
}

// Push interleaved int16 frames to the stream: a local copy into the shared
// ring plus atomic index/volume updates.  No main-thread round trip — this
// runs entirely on the emulation thread (perf proposal P6).
void platform_audio_push(const int16_t *frames, int nframes, int vol_0_7) {
    if (!frames || nframes <= 0)
        return;
    gs_audio_ring_t *ring = &g_aring;
    int ch = g_aring_channels;
    uint32_t n = (uint32_t)nframes;
    uint32_t mask = GS_ARING_FRAMES - 1;
    if (n > mask)
        return; // nonsensical push (larger than the ring)

    // Silence detection (all samples equal — any DC level counts); this scan
    // used to run on the main thread inside the push proxy.
    int total = nframes * ch;
    bool silent = true;
    for (int k = 1; k < total; k++) {
        if (frames[k] != frames[0]) {
            silent = false;
            break;
        }
    }

    uint32_t w = atomic_load_explicit(&ring->write_idx, memory_order_relaxed);
    uint32_t r = atomic_load_explicit(&ring->read_idx, memory_order_acquire);
    uint32_t used = (w - r) & mask;
    uint32_t free_frames = mask - used;
    if (n > free_frames) {
        // Overwrite-oldest (matches the old worklet-side behaviour): advance
        // read_idx past the frames about to be clobbered.  CAS so a racing
        // consumer update isn't stomped; on failure the consumer freed space.
        uint32_t need = n - free_frames;
        uint32_t r_new = (r + need) & mask;
        atomic_compare_exchange_strong(&ring->read_idx, &r, r_new);
    }

    // Copy the frames in at most two contiguous spans.
    uint32_t first = GS_ARING_FRAMES - w;
    if (first > n)
        first = n;
    memcpy(&ring->data[w * ch], frames, (size_t)first * ch * sizeof(int16_t));
    if (n > first)
        memcpy(&ring->data[0], frames + first * ch, (size_t)(n - first) * ch * sizeof(int16_t));

    // Publish: data first, then the release-store of write_idx the consumer
    // acquires — the worklet never reads frames it can't see completely.
    atomic_store_explicit(&ring->write_idx, (w + n) & mask, memory_order_release);
    atomic_store_explicit(&ring->vol, vol_0_7 & 7, memory_order_relaxed);
    if (silent)
        atomic_fetch_add_explicit(&ring->silent_pushes, 1, memory_order_relaxed);
    else
        atomic_store_explicit(&ring->silent_pushes, 0, memory_order_relaxed);
    atomic_fetch_add_explicit(&ring->push_seq, 1, memory_order_relaxed);
    g_last_push_time = host_time();
}

// Change the source sample rate mid-stream
void platform_audio_set_rate(uint32_t src_rate_hz) {
    gs_audio_set_rate((int)src_rate_hz);
}

// Ring fill fraction against target depth for the accelerated-mode governor:
// an atomic read of the worklet's periodic report in the ring header.
// Meaningful only while the producer is actively pushing — reads as "no
// signal" within half a second of the stream idling, matching the governor's
// contract from the proxy-based implementation.
double platform_audio_ring_fill(void) {
    if (g_last_push_time < 0.0 || host_time() - g_last_push_time > 0.5)
        return -1.0;
    int32_t pm = atomic_load_explicit(&g_aring.fill_pm, memory_order_relaxed);
    if (pm < 0)
        return -1.0;
    return (double)pm / 1000.0;
}
