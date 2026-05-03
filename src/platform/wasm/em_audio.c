// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// em_audio.c
// Audio subsystem for Emscripten platform - implements WebAudio streaming via AudioWorklet with ring buffer
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

// WebAudio (AudioWorklet) bridge — low-jitter streaming via MessagePort.
// Audio format input: 8-bit unsigned mono at ~22.2545 kHz.

// Initialize the WebAudio subsystem and create AudioWorklet
// clang-format off
EM_JS(void, mplus_audio_init_js, (), {
    // Check if already initialized
    if (Module.mPlusAudio && Module.mPlusAudio.initialized)
        return;

    // Initialize audio subsystem object
    var mp = Module.mPlusAudio = (Module.mPlusAudio || {});
    mp.initialized = true;
    mp.srcRate = 22254.5;
    mp.vblSamples = 370;
    mp.bufferVBLs = 5; // fixed latency target
    mp.targetSamples = mp.vblSamples * mp.bufferVBLs; // 1850
    mp.targetLatency = mp.targetSamples / mp.srcRate; // ~83ms
    mp.ctx = mp.ctx || new (self.AudioContext || self.webkitAudioContext)();

    // AudioWorklet processor — internal ring buffer fed via MessagePort.
    var RING_BYTES = 1 << 17; // 128 KiB ~ 5.7s at source rate
    var workletCode = [
        "class MPlusProcessor extends AudioWorkletProcessor {",
        "  constructor(opts){",
        "    super();",
        "    var o=opts.processorOptions;",
        "    var RB=" + RING_BYTES + ";",
        "    this.data=new Uint8Array(RB);",
        "    this.mask=RB-1;",
        "    this.wIdx=0; this.rIdx=0;",
        "    this.vol=0; this.silentCount=0; this.underruns=0;",
        "    this.srcRate=o.srcRate||22254.5;",
        "    this.targetLatency=o.targetLatency||0.083;",
        "    this.vblSamples=o.vblSamples||370;",
        "    this.targetSamples=Math.floor(this.targetLatency*this.srcRate);",
        "    this.dstRate=sampleRate;",
        "    this.step=this.srcRate/this.dstRate;",
        "    this.errI=0; this.frac=0; this.lpfY=0;",
        "    var fc=8000; this.lpfA=1.0-Math.exp(-2*Math.PI*fc/this.dstRate);",
        "    this.curGain=0; this.started=false;",
        "    var self2=this;",
        "    this.port.onmessage=function(e){",
        "      var d=e.data; var src=new Uint8Array(d.buf);",
        "      var len=src.length;",
        "      self2.vol=d.vol;",
        "      if(d.sil){self2.silentCount++;}else{self2.silentCount=0;}",
        "      var w=self2.wIdx, r=self2.rIdx, mask=self2.mask;",
        "      var used=(w-r)&mask, free=mask-used;",
        "      if(len>free){self2.rIdx=(r+len-free)&mask;}",
        "      for(var i=0;i<len;i++){self2.data[(w+i)&mask]=src[i];}",
        "      self2.wIdx=(w+len)&mask;",
        "    };",
        "  }",
        "  process(inputs,outputs){",
        "    var out=outputs[0][0]; var frames=out.length;",
        "    var w=this.wIdx, r=this.rIdx, mask=this.mask;",
        "    var avail=(w-r)&mask;",
        "    if(!this.started){if(avail<this.targetSamples){for(var i=0;i<frames;i++)out[i]=0;return true;}this.started=true;}",
        "    var ts=this.targetSamples;",
        "    var targetGain=Math.min(Math.max(this.vol,0),7)/7;",
        "    var error=avail-ts;",
        "    this.errI=this.errI*0.995+error*0.005;",
        "    var adj=(error/ts)*0.005+(this.errI/ts)*0.001;",
        "    if(adj>0.002)adj=0.002;else if(adj<-0.002)adj=-0.002;",
        "    var stepAdj=this.step*(1+adj);",
        "    var gainStep=(targetGain-this.curGain)/frames;",
        "    for(var i=0;i<frames;i++){",
        "      var have=(w-r)&mask;",
        "      if(have<2){this.underruns++;this.curGain+=gainStep;out[i]=0;continue;}",
        "      var i0=this.frac|0; var i1=i0+1; if(i1>=have)i1=have-1;",
        "      var b0=this.data[(r+i0)&mask];",
        "      var b1=this.data[(r+i1)&mask];",
        "      var frac=this.frac-i0;",
        "      var x=((b0-128)/128)+(((b1-128)/128)-((b0-128)/128))*frac;",
        "      this.lpfY+=this.lpfA*(x-this.lpfY);",
        "      this.curGain+=gainStep;",
        "      out[i]=this.lpfY*this.curGain;",
        "      this.frac+=stepAdj;",
        "      var consumed=this.frac|0;",
        "      if(consumed>0){this.frac-=consumed;var wc=consumed<have?consumed:have;r=(r+wc)&mask;}",
        "    }",
        "    this.rIdx=r;",
        "    if(this.silentCount>=8){",
        "      var depth=(this.wIdx-this.rIdx)&mask;",
        "      if(depth>ts){this.rIdx=(this.rIdx+depth-ts)&mask;}",
        "    }",
        "    return true;",
        "  }",
        "}",
        "registerProcessor('mplus-worklet',MPlusProcessor);"
    ].join("\n");

    // Create worklet from blob URL
    var blobURL = URL.createObjectURL(new Blob([workletCode], {type: 'application/javascript'}));
    mp.ctx.audioWorklet.addModule(blobURL)
        .then(function() {
            try {
                URL.revokeObjectURL(blobURL);
            } catch (e) {
            }
            try {
                mp.node = new AudioWorkletNode(mp.ctx, 'mplus-worklet', {
                    numberOfInputs: 0,
                    numberOfOutputs: 1,
                    outputChannelCount: [1],
                    processorOptions: {
                        srcRate: mp.srcRate,
                        targetLatency: mp.targetLatency,
                        vblSamples: mp.vblSamples
                    }
                });
                mp.node.connect(mp.ctx.destination);
                console.log('[audio] worklet init (MessagePort) targetLatency=' + (mp.targetLatency * 1000).toFixed(1) + 'ms');
            } catch (e) {
                console.error('[audio] worklet node creation failed', e);
            }
        })
        .catch(function(e) { console.error('[audio] addModule failed', e); });
});

// Resume audio context if suspended
EM_JS(void, mplus_audio_resume_js, (), {
    try {
        const mp = Module.mPlusAudio;
        if (mp && mp.ctx && mp.ctx.state === 'suspended')
            mp.ctx.resume();
    } catch (e) {
    }
});

// Push audio data to worklet via MessagePort
EM_JS(void, mplus_audio_push_js, (uint32_t ptr, int len, int volume), {
    var mp = Module.mPlusAudio;
    if (!mp || !mp.ctx || !mp.node || len <= 0)
        return;
    mplus_audio_resume_js();

    var heap = (typeof HEAPU8 !== 'undefined') ? HEAPU8 : Module.HEAPU8;
    if (!heap)
        return;
    var end = Math.min(ptr + len, heap.length);
    if (ptr >= end)
        return;

    // Copy samples from WASM heap (must copy — heap can grow/move)
    var samples = heap.slice(ptr, end);

    // Silence detection (all bytes equal)
    var silent = true;
    for (var k = 1; k < len; k++) {
        if (samples[k] !== samples[0]) {
            silent = false;
            break;
        }
    }

    // Send to worklet via MessagePort (transfer the underlying buffer)
    mp.node.port.postMessage(
        {buf: samples.buffer, vol: (volume | 0) & 7, sil: silent},
        [samples.buffer]
    );
});
// clang-format on

// ============================================================================
// Main-thread Proxy Wrappers
// ============================================================================

// These wrappers ensure audio EM_JS functions execute on the main thread
// (where AudioContext and AudioWorklet are available), even though the
// emulator runs on a worker thread via PROXY_TO_PTHREAD.

static void mplus_audio_init(void) {
    if (emscripten_is_main_browser_thread()) {
        mplus_audio_init_js();
    } else {
        emscripten_sync_run_in_main_runtime_thread(EM_FUNC_SIG_V, mplus_audio_init_js);
    }
}

static void mplus_audio_resume(void) {
    if (emscripten_is_main_browser_thread()) {
        mplus_audio_resume_js();
    } else {
        emscripten_sync_run_in_main_runtime_thread(EM_FUNC_SIG_V, mplus_audio_resume_js);
    }
}

static void mplus_audio_push(uint32_t ptr, int len, int volume) {
    if (emscripten_is_main_browser_thread()) {
        mplus_audio_push_js(ptr, len, volume);
    } else {
        emscripten_sync_run_in_main_runtime_thread(EM_FUNC_SIG_VIII, mplus_audio_push_js, ptr, len, volume);
    }
}

// ============================================================================
// Operations (Public API)
// ============================================================================

// Initialize audio subsystem
void em_audio_init(void) {
    mplus_audio_init();
}

// Resume audio context
void em_audio_resume(void) {
    mplus_audio_resume();
}

// Play 8-bit PWM audio samples
void em_audio_play_8bit_pwm(uint8_t *samples, int num_samples, unsigned int volume) {
    if (!samples || num_samples <= 0)
        return;
    mplus_audio_push((uint32_t)(uintptr_t)samples, num_samples, (int)volume);
}

// ============================================================================
// Platform Interface Implementation
// ============================================================================

// Platform interface implementation for sound module
void platform_init_sound(void) {
    em_audio_init();
}

// Platform interface implementation for PWM playback
void platform_play_8bit_pwm(uint8_t *samples, int num_samples, unsigned int volume) {
    em_audio_play_8bit_pwm(samples, num_samples, volume);
}

// ============================================================================
// Shell Commands
// ============================================================================

// Phase 5c — legacy `beep` shell command and its handler retired.
