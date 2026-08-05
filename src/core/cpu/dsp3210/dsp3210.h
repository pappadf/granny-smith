// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// dsp3210.h
// AT&T DSP3210 auxiliary-CPU core — the floating-point DSP in the Quadra
// 840AV / Centris 660AV.  Adapted from the validated reference core in
// local/gs-docs/dsp3210/dsp3210emu-fp (exact integer DAU variant), per the
// core-module contract in docs/core/cpu/cores.md.
//
// Sources of truth:
//   - AT&T "DSP3210 Information Manual" (Sept 1991): ch. 4 instruction pages,
//     ch. 7 exception model, ch. 8 DAU + DSP32 float format, ch. 9 timer/BIO,
//     ch. 10 encodings.
//   - The ROM-verified dossier local/gs-docs/840av_660av/docs/dsp3210.md
//     (§1.4 MMIO map, §1.5 encodings, §1.6 exceptions) and the errata log
//     local/gs-docs/dsp3210/ERRATA-and-emulator-bugs.md — every semantic
//     hardened there (irsh replay under the pre-switch memory map, masked
//     address-error completion, CA-load flags, IEEE↔DSP32 bias, guard bits)
//     is load-bearing: real Apple/AT&T code found each one.
//
// Scope:
//   - Complete instruction-set decode; every one of the 64 top-level opcodes
//     is executed or raises the documented illegal-opcode error.
//   - CA instructions to the letter of the manual (delayed branches,
//     do-loops, hard-wired r0, pc = insn+8, 16-bit flag rules).
//   - Exact integer DAU: 40-bit accumulators (24-bit mantissa + 8 guard bits
//     + 8-bit exponent), exact 25x25 multiplier, truncating adder, documented
//     rounding.  No host floating point in the data path.
//   - Exception model: 16-entry vector table at evtp, error vs interrupt
//     dispatch, emr masking, processing levels, ireturn with shadow restore,
//     waiti, sftrst.  bkpt halts the core (state "crashed").
//   - On-chip peripherals the Mac sound path uses ARE modelled: the 32-bit
//     timer (tcon/count, vector 9, incl. the masked-address-error
//     (long)-store-to-$413 path of the errata) and the BIO port (bio/bioc,
//     2-bit op fields, output transitions surfaced through a callback — the
//     AV board's DSP→host doorbell).  SIO/DMAC registers are
//     present-but-inert (plain on-chip RAM).
//   - PS.IR0/IR1 read the LIVE EXT0/EXT1 pin level (1 = negated): the board
//     delivers short active-low pulses via dsp3210_ext_pulse(), and the RTM
//     kernel polls `ir1s` both to measure the frame period (calibration
//     gadget) and to detect frame overrun between modules.  Writing emr with
//     bit 0 set drops a latched-but-untaken EXT1 request (the kernel's
//     "clear the edge latch" pulse, dsp-kernel-messages.md §3.5).
//
// Portable C99, no globals, no I/O, no allocation: the struct is plain data
// followed by pointers (checkpoint boundary at `mem`).  Bus access outside
// the on-chip window goes through injected hooks; the core never touches the
// main CPU's memory globals (cores.md contract).

#ifndef GS_CPU_DSP3210_H
#define GS_CPU_DSP3210_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// step() return status
enum {
    DSP3210_STEP_OK = 0, // executed one instruction
    DSP3210_STEP_WAITI, // in wait-for-interrupt, nothing pending
    DSP3210_STEP_BKPT, // executed bkpt (spc = (short) r0) — core halts
    DSP3210_STEP_DERROR // double error — only reset recovers
};

// processing levels [IM §7.5]
enum { DSP3210_LVL_BASE = 0, DSP3210_LVL_INTERRUPT, DSP3210_LVL_ERROR, DSP3210_LVL_DERROR };

// exception vector numbers [IM Figure 7-5]
enum {
    DSP3210_VEC_RESET = 0,
    DSP3210_VEC_BUSERR = 1, // non-maskable
    DSP3210_VEC_ILLOP = 2, // non-maskable
    DSP3210_VEC_AERR = 4, // maskable, emr[4]
    DSP3210_VEC_UV = 5, // DAU overflow/underflow, emr[5]
    DSP3210_VEC_NAN = 6, // IEEE NaN on dsp(), emr[6]
    DSP3210_VEC_EXT0 = 8,
    DSP3210_VEC_TIMER = 9,
    DSP3210_VEC_IBF = 11,
    DSP3210_VEC_OBE = 12,
    DSP3210_VEC_IFRM = 13,
    DSP3210_VEC_OFRM = 14,
    DSP3210_VEC_EXT1 = 15
};

// ps flag bits [IM Table 10-4]
#define DSP3210_PS_n   (1u << 0) // CAU negative
#define DSP3210_PS_z   (1u << 1) // CAU zero
#define DSP3210_PS_v   (1u << 2) // CAU overflow
#define DSP3210_PS_c   (1u << 3) // CAU carry/borrow
#define DSP3210_PS_N   (1u << 4) // DAU negative
#define DSP3210_PS_Z   (1u << 5) // DAU zero
#define DSP3210_PS_U   (1u << 6) // DAU underflow
#define DSP3210_PS_V   (1u << 7) // DAU overflow
#define DSP3210_PS_IBF (1u << 8)
#define DSP3210_PS_OBE (1u << 9)
#define DSP3210_PS_SY  (1u << 10)
#define DSP3210_PS_FB  (1u << 11)
#define DSP3210_PS_IR0 (1u << 12) // live EXT0 pin level (1 = negated)
#define DSP3210_PS_IR1 (1u << 13) // live EXT1 pin level (1 = negated)

// Injected bus hooks for everything OUTSIDE the on-chip 64 KB window (the
// on-chip window — RAM, MMIO, boot-ROM range — decodes inside the core
// first, per the cores.md contract).  size is 1, 2 or 4; addresses are
// already alignment-handled.  Set *fault nonzero to signal a bus error.
typedef uint32_t (*dsp3210_read_fn)(void *ctx, uint32_t addr, int size, int *fault);
typedef void (*dsp3210_write_fn)(void *ctx, uint32_t addr, uint32_t val, int size, int *fault);

// BIO output transition: `old_pins`/`new_pins` are the driven pin levels
// (output register ANDed with the bioc direction mask).  The AV glue latches
// PSC L5 bit 0 on any BIO0 transition — the RTM's per-message doorbell.
typedef void (*dsp3210_bio_fn)(void *ctx, uint8_t old_pins, uint8_t new_pins);

struct dsp3210;

// A 40-bit DAU accumulator, modelled exactly [IM Figure 8-8B, §8.2.3]:
//
//   40-bit word = mantissa (bits 39-16) | guard (15-8) | exponent (7-0)
//
// The mantissa+guard field (bits 39-8) is a single 2's complement quantity
// `s (!s) . f31…f0` — the implicit leading bit is !s, so the represented
// mantissa M lies in [1,2) or [-2,-1).  `m` holds that value *including*
// the implicit bit, as a fixed-point integer with 31 fractional bits:
//
//   value = m * 2^(e - 128) / 2^31,   |m| in [2^31, 2^32)  when normal
//   value = 0                                              when e == 0
//
// Every DAU operation is carried out on these integers: the multiplier is
// an exact 25x25-bit integer multiply, the adder aligns and truncates
// exactly as the hardware does, and only `round` rounds.
typedef struct {
    int64_t m; // mantissa+guard incl. implicit bit, 31 frac bits
    int16_t e; // biased exponent; 0 means the value is zero
} dsp3210_acc;

// Accumulator <-> host double, for tests and diagnostics only.
double dsp3210_acc_double(dsp3210_acc a);
dsp3210_acc dsp3210_acc_from_double(double v);
// Accumulator <-> the packed 32-bit DSP32 word (guard bits truncated).
uint32_t dsp3210_acc_pack(dsp3210_acc a);
dsp3210_acc dsp3210_acc_unpack(uint32_t w);

// Uniform accessors (tests and the object node).
double dsp3210_acc_get(const struct dsp3210 *s, int n);
void dsp3210_acc_set(struct dsp3210 *s, int n, double v);
// Raw 40-bit accumulator fields (stored bits 39-8 + exponent).
void dsp3210_acc_raw(const struct dsp3210 *s, int n, int64_t *mant_guard, int *exp);

typedef struct dsp3210 {
    // --- plain data (checkpointed up to `mem`) ---

    // CAU registers
    uint32_t r[23]; // r0 (hardwired 0)..r22; r20 = error trace, r21 = sp, r22 = evtp
    uint32_t pc; // address of the next instruction to execute
    uint32_t npc; // the one after (a taken delayed branch sets npc)
    uint32_t pcsh; // program counter shadow: resume address + 4; the
                   // not-yet-executed instruction at the resume address is
                   // held in irsh and replayed by ireturn [IM §7.5.1]

    // instruction shadow register: the word prefetched before an interrupt
    // (under the pre-interrupt memory map — the boot ROM's SAR path depends
    // on that), replayed by ireturn
    uint32_t irsh, irsh_addr;
    int irsh_pending;
    uint32_t prefetch, prefetch_addr; // last word prefetched at npc

    // IO registers
    uint16_t ps; // processor status (flags)
    uint16_t emr; // exception mask register
    uint16_t pcw; // processor control word
    uint8_t dauc; // DAU control
    uint8_t ctr; // clip-test register (DAU N history)
    int pcw_locked; // pcw[13] written 1 → locked until reset/error

    // DAU accumulators (exact 40-bit model)
    dsp3210_acc a[4];

    // do-loop state [IM DO page]
    int do_active;
    int do_lock; // dolock: interrupts disabled for the loop
    uint32_t do_start, do_end, do_count;

    // exception state [IM §7.5]
    int level; // DSP3210_LVL_*
    int error_nonmaskable; // type of error being processed
    uint16_t pending; // pending interrupts, bit n = vector n
    int waiting; // in waiti
    int int_defer; // run one insn before taking interrupt (waiti latent insn)
    int last_vector; // last vector raised (incl. masked)
    int halted; // bkpt executed — dead until reset ("crashed")
    // live EXT0/EXT1 pin windows: remaining instruction-slots the pin stays
    // asserted after dsp3210_ext_pulse() (0 = negated).  PS.IR0/IR1 read the
    // LIVE pin level (1 = negated) — the RTM kernel's frame-period gadget
    // and per-module overrun polls both spin on `ir1s` waiting for the
    // board's short active-low frame pulse (dsp-kernel-messages.md §3.2/3.4)
    uint32_t ext_pulse[2]; // [0] = EXT0 (vec 8), [1] = EXT1 (vec 15)

    // interrupt shadow registers [IM §7.5.1; ps/ctr per Figure 4-1]
    uint16_t sh_ps;
    uint8_t sh_dauc, sh_ctr;
    dsp3210_acc sh_a[4];
    int sh_do_active, sh_do_lock;
    uint32_t sh_do_start, sh_do_end, sh_do_count;

    // on-chip timer [IM §9.3]: writing `timer` loads counter AND reload;
    // reaching zero raises vector 9; auto-reload period = N+1 ticks
    uint8_t tcon; // $50030413: E/HN, R/SN, T/PN, H/LN, OUT, SRC[7:5]
    uint32_t timer_count; // $50030414 live down-counter
    uint32_t timer_reload; // initial-count register

    // on-chip BIO port [IM §9.4]: 8 GPIO pins, no interrupt capability
    uint8_t bioc; // direction, 1 = output (reset: all inputs)
    uint8_t bio_out; // output-register bits (reset: 0)

    // bookkeeping
    uint64_t icount;
    uint32_t cur_insn; // address of the instruction being executed

    // on-chip 64 KB window: at $50030000-$5003FFFF in processor mode
    // (pcw[10]=0); in computer mode the window is $0000xxxx.  Boot-ROM
    // range and unmodelled MMIO words are plain RAM.
    uint8_t chip[0x10000];

    // --- pointers (not checkpointed; the owner re-plants after restore) ---
    uint8_t *mem; // optional flat external memory at address 0 (rigs/tests)
    uint32_t mem_size;
    dsp3210_read_fn read_fn; // external-bus hooks (replace `mem` when set)
    dsp3210_write_fn write_fn;
    void *hook_ctx;
    dsp3210_bio_fn bio_fn; // BIO output-transition callback (may be NULL)
    void *bio_ctx;
} dsp3210_t;

// Checkpoint boundary: everything before `mem` is plain data.
#define DSP3210_CHECKPOINT_SIZE offsetof(dsp3210_t, mem)

// Initialise: attach a flat memory buffer (may be NULL/0 if hooks are used)
// and perform a hardware reset.  straps = the BIO7..BIO4 reset straps as the
// value latched into pcw[10..13]: bit 0 → pcw[10] (C/PN), bit 3 → pcw[13]
// (BRC).  The AV Macs strap 0 on bit 0 (processor mode); pass 0 for that.
void dsp3210_init(dsp3210_t *s, uint8_t *mem, uint32_t mem_size);
void dsp3210_reset(dsp3210_t *s, unsigned straps);

// Execute one instruction (or report the blocked state).
int dsp3210_step(dsp3210_t *s);

// Burn-down execution (the aux-core sprint ABI, cores.md): run until the
// budget drains or the core goes idle.  A waiti sleep with the timer running
// consumes budget (the timer keeps counting toward its wake-up); a sleep
// with no wake source, a bkpt halt or a double error leave the budget
// unspent — the caller parks the burst event.
void dsp3210_run(dsp3210_t *s, uint32_t *instructions);

// True when running the core cannot make progress: halted (bkpt), double
// error, or waiti with nothing pending and no timer ticking.
int dsp3210_is_idle(const dsp3210_t *s);

// Assert an interrupt request (vector 8..15).  It is taken when the
// corresponding emr bit is set and the processor is at base level.  The
// PS.IR0/IR1 pin mirrors are NOT touched — board pulses go through
// dsp3210_ext_pulse().
void dsp3210_request_interrupt(dsp3210_t *s, int vector);

// Deliver a short active-low pulse on the EXT0 or EXT1 pin (vector 8 or
// 15): latches the interrupt request (edge) and asserts the PS.IR pin
// mirror for `slots` instruction-slots of core time (executed instructions
// and waiti sleep slots both count), then the pin reads negated again.
void dsp3210_ext_pulse(dsp3210_t *s, int vector, uint32_t slots);

// Copy bytes into emulated memory through the address map (for loading
// program images).  Returns 0, or -1 if part of the range is unmapped.
int dsp3210_load(dsp3210_t *s, uint32_t addr, const void *buf, size_t len);

// Raw memory peek/poke through the address map (endianness per pcw[8]).
// Return 0 on success, -1 on unmapped/misaligned.  No exceptions raised, no
// MMIO side effects (MMIO words read their RAM shadow).
int dsp3210_peek(dsp3210_t *s, uint32_t addr, int size, uint32_t *out);
int dsp3210_poke(dsp3210_t *s, uint32_t addr, int size, uint32_t val);

// DSP32 32-bit floating-point format [IM §3.4.2] <-> host double.
double dsp3210_dsp32_to_double(uint32_t w);
uint32_t dsp3210_double_to_dsp32(double v);

const char *dsp3210_step_name(int status);

#ifdef __cplusplus
}
#endif

#endif // GS_CPU_DSP3210_H
