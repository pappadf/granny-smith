// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// platform_hooks.h
// The platform seams: functions the core calls that the host platform
// (src/platform/wasm, src/platform/headless) may provide.
//
// Contract.  Every hook here has a weak default in platform_hooks.c that
// models "this host has none of it" -- no camera, no GPU worker, no
// auto-checkpoint loop -- and a platform that has the thing links a strong
// definition of the same name.  A default that is asked to DO something it
// cannot answers -2 ("not supported on this platform"); 0 is success and
// other negative values are failures of a platform that tried.  The `gs_`
// prefix marks a hook of this contract (Granny Smith's platform surface);
// core-owned functions use `system_`.

#ifndef PLATFORM_HOOKS_H
#define PLATFORM_HOOKS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A new machine has become the active one (boot or restore).  The host
// re-bases its samples of the machine and announces what it shows of it; the
// weak default serves a host that samples nothing.
void platform_machine_attached(void);

// True when `path` lives on volatile scratch storage (the browser's memfs
// /tmp, where uploaded test media land): a writable mount of such an image
// keeps its delta in the image scratch root, not in the machine directory.
bool gs_path_is_volatile(const char *path);

// Background auto-checkpoint state.  The wasm platform overrides the weak
// defaults to read/write its live flag; a platform with no auto-checkpoint
// loop (headless) reads false and refuses the set (-2, "not supported").
bool gs_checkpoint_auto_get(void);
int gs_checkpoint_auto_set(bool enabled);

// Platform-specific entry points used by typed root methods: the weak default
// answers -2 ("not supported on this platform"), and the platform that has
// the thing overrides it.  0 on success.
//
//   gs_quit()                — request emulator shutdown.  Headless sets
//                              the quit flag; the browser owns the page.
//   gs_download(path)        — hand a file to the browser as a download
//                              (wasm, via blob+anchor); headless has none.
int gs_quit(void);
int gs_download(const char *path);

// Host video-input seam (the AV video digitizer's webcam source).  The weak
// defaults model "no camera": headless machines use the deterministic
// machine.videoin sources instead; the WASM platform overrides these with the
// getUserMedia frame path (em_camera.c).
//
//   gs_video_in_connected()  — true when a host camera is attached and
//                              delivering frames (drives the DMSD's
//                              signal-lock status bit).
//   gs_video_in_frame(rgba)  — fill a 640x480 RGBA8888 top-down buffer
//                              with the current camera frame; returns 0,
//                              or -1 when no source is connected.
//   gs_video_in_state(active)— capture-engine on/off notification (the
//                              guest gating the VDC clock); the browser
//                              attaches/stops the camera track on it.
bool gs_video_in_connected(void);
int gs_video_in_frame(uint8_t *rgba);
void gs_video_in_state(bool active);

// Host GPU-transport seam for the Voodoo2's WebGPU takeover
// (voodoo2_gpu.c): the browser build attaches a GPU worker to a shared
// region in the wasm heap and wakes it through Atomics; the defaults
// model "no GPU" so native builds fall back to the thread backend.  `ctrl`
// is the region's control block (voodoo2_gpu_protocol.h); `wait` blocks the
// CALLING pthread until *addr != expected or the timeout (returns 0 when
// woken).
bool gs_v2gpu_available(void);
bool gs_v2gpu_attach(void *ctrl, uint32_t bytes);
void gs_v2gpu_detach(void *ctrl);
int gs_v2gpu_wait(volatile uint32_t *addr, uint32_t expected, uint32_t timeout_ms);
void gs_v2gpu_notify(volatile uint32_t *addr);

// Host audio-input seam (the AV Singer codec's microphone source,
// mirroring the video-in seam).  The weak defaults model "no microphone":
// headless machines use the deterministic machine.audioin sources instead; a
// browser platform override (getUserMedia audio) can trail in a follow-up.
//
//   gs_audio_in_connected()      — true when a host microphone is
//                                  attached and delivering samples
//                                  (drives the singerStat mic sense).
//   gs_audio_in_frames(lr,n,rate)— fill `n` interleaved stereo int16
//                                  sample pairs at `rate` Hz from the
//                                  current source; returns false when no
//                                  source is connected (buffer untouched
//                                  — the caller keeps silence).
//   gs_audio_in_state(active)    — capture on/off notification (the
//                                  guest gating pSndInEn).
bool gs_audio_in_connected(void);
bool gs_audio_in_frames(int16_t *lr, uint32_t frames, uint32_t rate);
void gs_audio_in_state(bool active);
// Optional one-line description of the host capture's own state, appended to
// machine.audioin's level meter.  The guest-side level alone cannot say
// WHERE audio was lost — the platform knows whether samples arrived at all.
// Weak default writes nothing and returns false.
bool gs_audio_in_debug(char *buf, size_t buflen);
// machine.audioin.inject notification: the platform may MONITOR the injected
// file through its own speakers so a demo audience hears what the guest was
// just fed.  Pure UX — the emulated input path is untouched (the browser
// plays the same file from its own storage).  Weak default: silent.
void gs_audio_in_injected(const char *path);

#endif // PLATFORM_HOOKS_H
