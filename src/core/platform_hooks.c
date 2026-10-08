// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// platform_hooks.c
// The weak defaults of the platform seams (platform_hooks.h): each models a
// host that has none of the thing, and a platform that has it links a strong
// definition of the same name.

#include "platform_hooks.h"

#include <string.h>

// A new machine is the active one (machine.boot, checkpoint.load).  The host
// re-bases whatever it samples from the machine: nothing it observed of the
// previous machine is compared with this one (the page's MIPS sample, headless
// --max-cycles' count).  The weak default serves a host that samples nothing.
__attribute__((weak)) void platform_machine_attached(void) {}

// The browser's memfs scratch: test media uploaded under /tmp/ keep their
// deltas in the image scratch root (also /tmp), preserving memfs-only I/O.
// The headless build shares the rule, which is why it is the default.
__attribute__((weak)) bool gs_path_is_volatile(const char *path) {
    return path && strncmp(path, "/tmp/", 5) == 0;
}

// Background-checkpoint auto state.  The headless build has no auto-checkpoint
// loop, so the defaults stub out; em_main.c overrides them to read/write the
// live `checkpoint_auto_enabled` flag.
__attribute__((weak)) bool gs_checkpoint_auto_get(void) {
    return false;
}

__attribute__((weak)) int gs_checkpoint_auto_set(bool enabled) {
    (void)enabled;
    return -2; // no auto-checkpoint loop on this platform
}

// Platform-specific entry points (see platform_hooks.h): the weak defaults say "not
// supported on this platform" (-2), and a platform that has the thing
// overrides them -- headless quit, wasm download.
__attribute__((weak)) int gs_quit(void) {
    return -2; // the browser owns the page's lifecycle
}
__attribute__((weak)) int gs_download(const char *path) {
    (void)path;
    return -2; // no browser to hand a file to
}

// Host video-input seam: the defaults model "no camera attached" — the
// headless build drives capture from the deterministic machine.videoin
// sources instead; em_camera.c overrides these on WASM.
__attribute__((weak)) bool gs_video_in_connected(void) {
    return false;
}

__attribute__((weak)) int gs_video_in_frame(uint8_t *rgba) {
    (void)rgba;
    return -1;
}

__attribute__((weak)) void gs_video_in_state(bool active) {
    (void)active;
}

// Host GPU-transport seam (platform_hooks.h): no GPU on a native host.
__attribute__((weak)) bool gs_v2gpu_available(void) {
    return false;
}

__attribute__((weak)) bool gs_v2gpu_attach(void *ctrl, uint32_t bytes) {
    (void)ctrl;
    (void)bytes;
    return false;
}

__attribute__((weak)) void gs_v2gpu_detach(void *ctrl) {
    (void)ctrl;
}

__attribute__((weak)) int gs_v2gpu_wait(volatile uint32_t *addr, uint32_t expected, uint32_t timeout_ms) {
    (void)addr;
    (void)expected;
    (void)timeout_ms;
    return -1;
}

__attribute__((weak)) void gs_v2gpu_notify(volatile uint32_t *addr) {
    (void)addr;
}

// Host audio-input seam: the defaults model "no microphone attached" —
// the headless build drives capture from the deterministic
// machine.audioin sources instead; a WASM override can trail.
__attribute__((weak)) bool gs_audio_in_connected(void) {
    return false;
}

__attribute__((weak)) bool gs_audio_in_frames(int16_t *lr, uint32_t frames, uint32_t rate) {
    (void)lr;
    (void)frames;
    (void)rate;
    return false;
}

__attribute__((weak)) void gs_audio_in_state(bool active) {
    (void)active;
}

__attribute__((weak)) void gs_audio_in_injected(const char *path) {
    (void)path;
}

__attribute__((weak)) bool gs_audio_in_debug(char *buf, size_t buflen) {
    (void)buf;
    (void)buflen;
    return false;
}
