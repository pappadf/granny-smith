// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// em_audio.c
// Audio subsystem for Emscripten platform - implements WebAudio streaming via AudioWorklet with ring buffer

// ============================================================================
// Includes
// ============================================================================

#include "em.h"

#include <emscripten.h>
#include <emscripten/emscripten.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"

// ============================================================================
// WebAudio (AudioWorklet) Implementation
// ============================================================================

// WebAudio (AudioWorklet) bridge - High quality, low-jitter streaming
// Primary path: SharedArrayBuffer ring buffer (requires crossOriginIsolated)
// Fallback: message queue to worklet (copies chunks) when SAB not available
// Audio format input: 8-bit unsigned mono at ~22.2545 kHz.

// Initialize the WebAudio subsystem and create AudioWorklet
// clang-format off
EM_JS(void, mplus_audio_init, (), {
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

    // Create ring buffers (SAB) if available
    var RING_BYTES = 1 << 17; // 128 KiB ~ 5.7s at source rate
    var STATE_INT32S = 8; // [writeIdx, readIdx, volume, flags, underruns, lastDepthSamples, pad0, pad1]

    if (typeof SharedArrayBuffer === 'undefined') {
        console.warn('[audio] SharedArrayBuffer unavailable; cannot init worklet path');
        return;
    }

    mp.dataSAB = new SharedArrayBuffer(RING_BYTES);
    mp.stateSAB = new SharedArrayBuffer(STATE_INT32S * 4);
    mp.data = new Uint8Array(mp.dataSAB);
    mp.state = new Int32Array(mp.stateSAB);
    mp.mask = RING_BYTES - 1;

    // Flags bit0: paused, bit1: silenceMode (currently unused placeholder)
    var workletCode =
        "class MPlusProcessor extends AudioWorkletProcessor {\\n" + "  constructor(opts){\\n" + "    super();\\n" +
        "    var o = opts.processorOptions;\\n" + "    this.data=new Uint8Array(o.dataSAB);\\n" +
        "    this.state=new Int32Array(o.stateSAB);\\n" + "    this.mask=o.mask|0;\\n" +
        "    this.srcRate=o.srcRate||22254.5;\\n" + "    this.targetLatency=o.targetLatency||0.083;\\n" +
        "    this.vblSamples=o.vblSamples||370;\\n" +
        "    this.targetSamples=Math.floor(this.targetLatency*this.srcRate);\\n" + "    this.dstRate=sampleRate;\\n" +
        "    this.step=this.srcRate/this.dstRate; /* base step */\\n" +
        "    this.errI=0; /* integral term for depth control */\\n" + "    this.frac=0;\\n" + "    this.lpfY=0;\\n" +
        "    this.lpfA=0.0;\\n" + "    var fc=8000; var x=Math.exp(-2*Math.PI*fc/this.dstRate); this.lpfA=1.0-x;\\n" +
        "    this.curGain=0;\\n" + "    this.started=false;\\n" +

        "  }\\n" + "  process(inputs,outputs,params){\\n" + "    var out=outputs[0][0];\\n" +
        "    var frames=out.length;\\n" + "    var w=Atomics.load(this.state,0)|0;\\n" +
        "    var r=Atomics.load(this.state,1)|0;\\n" + "    var volume=Atomics.load(this.state,2)|0;\\n" +
        "    var flags=Atomics.load(this.state,3)|0;\\n" + "    var paused = (flags&1)!==0;\\n" +
        "    var avail = (w-r)&this.mask;\\n" +
        "    if(!this.started){ if(avail < this.targetSamples){ for(var i=0;i<frames;i++){ out[i]=0; } return true; } " +
        "this.started=true; }\\n" +

        "    var targetSamples = this.targetSamples;\\n" +
        "    var targetGain = Math.pow(Math.min(Math.max(volume,0),7)/7,1.0);\\n" +
        "    /* Micro rate trim: adjust effective step so ring depth hovers at targetSamples */\\n" +
        "    var target = targetSamples;\\n" + "    var error = avail - target; /* + => overfull, - => draining */\\n" +
        "    this.errI = this.errI*0.995 + error*0.005; /* slow integral */\\n" +
        "    var adj = (error/target)*0.005 + (this.errI/target)*0.001; /* proportional + integral */\\n" +
        "    if(adj>0.002) adj=0.002; else if(adj<-0.002) adj=-0.002; /* clamp +/-2000 ppm */\\n" +
        "    var stepAdj = this.step * (1 + adj);\\n" + "    var gainStep = (targetGain - this.curGain)/frames;\\n" +
        "    if(paused){ for(var i=0;i<frames;i++){ this.curGain+=gainStep; out[i]=0; } return true; }\\n" +
        "    for(var i=0;i<frames;i++){\\n" + "      var have=(w-r)&this.mask;\\n" +
        "      if(have < 2){ Atomics.add(this.state,4,1); this.curGain+=gainStep; out[i]=0; continue; }\\n" +
        "      var i0=(this.frac|0); var i1=i0+1; if(i1>=have) i1=have-1;\\n" +
        "      var b0=this.data[(r + i0)&this.mask];\\n" + "      var b1=this.data[(r + i1)&this.mask];\\n" +
        "      var frac=this.frac - i0;\\n" + "      var x=((b0-128)/128) + (((b1-128)/128)-((b0-128)/128))*frac;\\n" +
        "      this.lpfY += this.lpfA*(x - this.lpfY);\\n" + "      this.curGain += gainStep;\\n" +
        "      out[i]=this.lpfY * this.curGain;\\n" + "      this.frac += stepAdj;\\n" +
        "      var consumed=this.frac|0; if(consumed>0){ this.frac-=consumed; var willConsume = consumed < have ? " +
        "consumed : have; r = (r + willConsume) & this.mask; }\\n" +
        "    }\\n" + "    Atomics.store(this.state,1,r);\\n" +
        "    /* Silence-aware depth trim: the worklet (sole consumer) trims excess */\\n" +
        "    /* ring data during sustained silence to prevent latency buildup.     */\\n" +
        "    var silentPushes = Atomics.load(this.state,5)|0;\\n" + "    if(silentPushes >= 8){\\n" +
        "      var w2=Atomics.load(this.state,0)|0; var r2=Atomics.load(this.state,1)|0;\\n" +
        "      var depth=(w2-r2)&this.mask;\\n" + "      if(depth > targetSamples){\\n" +
        "        var trim=depth-targetSamples;\\n" + "        Atomics.store(this.state,1,(r2+trim)&this.mask);\\n" +
        "      }\\n" + "    }\\n" + "    return true;\\n" + "  }\\n" + "}\\n" +
        "registerProcessor('mplus-worklet', MPlusProcessor);";

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
                        dataSAB: mp.dataSAB,
                        stateSAB: mp.stateSAB,
                        mask: mp.mask,
                        srcRate: mp.srcRate,
                        targetLatency: mp.targetLatency,
                        vblSamples: mp.vblSamples
                    }
                });
                mp.node.connect(mp.ctx.destination);
                console.log('[audio] worklet init targetLatency=' + (mp.targetLatency * 1000).toFixed(1) + 'ms');
            } catch (e) {
                console.error('[audio] worklet node creation failed', e);
            }
        })
        .catch(function(e) { console.error('[audio] addModule failed', e); });
});

// Resume audio context if suspended
EM_JS(void, mplus_audio_resume, (), {
    try {
        const mp = Module.mPlusAudio;
        if (mp && mp.ctx && mp.ctx.state === 'suspended')
            mp.ctx.resume();
    } catch (e) {
    }
});

// Push audio data into SAB ring buffer
EM_JS(void, mplus_audio_push, (uint32_t ptr, int len, int volume), {
    var mp = Module.mPlusAudio;
    if (!mp || !mp.ctx || !mp.data || !mp.state || len <= 0)
        return;
    mplus_audio_resume();

    var heap = (typeof HEAPU8 !== 'undefined') ? HEAPU8 : Module.HEAPU8;
    if (!heap)
        return;
    var end = Math.min(ptr + len, heap.length);
    if (ptr >= end)
        return;
    var src = heap.subarray(ptr, end);

    // Set volume
    Atomics.store(mp.state, 2, (volume | 0) & 7);

    // Silence detection (all bytes equal)
    var silent = 1;
    for (var k = 1; k < len; k++) {
        if (src[k] !== src[0]) {
            silent = 0;
            break;
        }
    }
    // Consecutive silent push count — lets worklet know when ring is all silence
    var sc = silent ? (Atomics.load(mp.state, 5) | 0) + 1 : 0;
    Atomics.store(mp.state, 5, sc);

    var w = Atomics.load(mp.state, 0) | 0;
    var r = Atomics.load(mp.state, 1) | 0;
    var mask = mp.mask;
    var capacity = mask + 1;
    var used = (w - r) & mask;
    var free = capacity - 1 - used;

    // Drop oldest data if ring is completely full (overflow)
    if (len > free) {
        var need = len - free;
        r = (r + need) & mask;
        Atomics.store(mp.state, 1, r);
    }

    // Always write data to ring — maintains stream continuity for the worklet
    // resampler/LPF so silence-to-sound transitions are glitch-free.
    var first = Math.min(len, capacity - (w & mask));
    for (var i = 0; i < first; i++)
        mp.data[(w + i) & mask] = src[i];
    for (var i2 = first; i2 < len; i2++)
        mp.data[(w + i2) & mask] = src[i2];
    w = (w + len) & mask;
    Atomics.store(mp.state, 0, w);
});
// clang-format on

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

// Shell command: beep - play a test tone
static uint64_t cmd_beep(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    // Generate a short 440Hz beep at source rate 22254.5 Hz
    const double rate = 22254.5;
    const double freq = 440.0;
    const double dur = 0.25; // 250 ms
    int samples = (int)(rate * dur);
    if (samples <= 0)
        samples = 1;

    uint8_t *buf = (uint8_t *)malloc(samples);
    if (!buf)
        return 0;

    // Generate sine wave
    for (int i = 0; i < samples; i++) {
        double t = (double)i / rate;
        double s = sin(2.0 * 3.141592653589793 * freq * t);
        int v = (int)(s * 127.0) + 128; // convert -1..1 to 0..255
        if (v < 0)
            v = 0;
        else if (v > 255)
            v = 255;
        buf[i] = (uint8_t)v;
    }

    platform_play_8bit_pwm(buf, samples, 7);
    free(buf);
    printf("(beep)\n");
    return 0;
}

// Registration hook for shell commands (called from em_main.c)
void em_audio_register_commands(void);
void em_audio_register_commands(void) {
    extern int register_cmd(const char *name, const char *category, const char *help, uint64_t (*func)(int, char **));
    register_cmd("beep", "Audio", "beep – play a test tone", cmd_beep);
}
