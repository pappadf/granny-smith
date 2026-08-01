// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

/*
 * dsp3210.c — AT&T DSP3210 auxiliary-CPU core
 *
 * Adapted from the validated reference core in
 * local/gs-docs/dsp3210/dsp3210emu-fp (exact integer DAU); see dsp3210.h
 * for provenance and scope.  Section references below ([IM §x.y],
 * instruction page names) are to the AT&T DSP3210 Information Manual;
 * [DOC §x] is local/gs-docs/840av_660av/docs/dsp3210.md.
 *
 * The interpreter is deliberately the simplest possible shape: one big
 * switch on the 6-bit top-level opcode, mirroring the reference
 * disassembler's decode exactly.  Additions over the reference core: the
 * on-chip timer and BIO port decode inside the core (with a BIO output
 * callback for the AV board's DSP→host doorbell), PS.IR0/IR1 mirror the
 * latched external requests, and dsp3210_run() provides the burn-down
 * sprint ABI of the core-module contract (docs/core/cpu/cores.md).
 */

#include "dsp3210.h"

#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* field helpers                                                       */

static uint32_t bits(uint32_t w, int hi, int lo) {
    return (w >> lo) & ((hi - lo == 31) ? 0xFFFFFFFFu : ((1u << (hi - lo + 1)) - 1u));
}

static int32_t sext16(uint32_t v) {
    return (int32_t)(int16_t)(v & 0xFFFFu);
}

/* ------------------------------------------------------------------ */
/* CAU register-code mapping (IM Table 10-2; deliberately discontinuous:
 * pc = 15, r15-r19 = 17-21, -n/+n = 22/23, r20-r22 = 24-26, pcsh = 30) */

enum { RK_ZERO, RK_REG, RK_PC, RK_PCSH, RK_PLUS, RK_MINUS, RK_RES };

typedef struct {
    uint8_t kind;
    uint8_t idx;
} regcode;

static regcode rc_decode(unsigned code) {
    regcode rc = {RK_RES, 0};
    code &= 31;
    if (code == 0) {
        rc.kind = RK_ZERO;
    } else if (code <= 14) {
        rc.kind = RK_REG;
        rc.idx = (uint8_t)code;
    } else if (code == 15) {
        rc.kind = RK_PC;
    } else if (code >= 17 && code <= 21) {
        rc.kind = RK_REG;
        rc.idx = (uint8_t)(code - 2);
    } else if (code == 22) {
        rc.kind = RK_MINUS;
    } else if (code == 23) {
        rc.kind = RK_PLUS;
    } else if (code >= 24 && code <= 26) {
        rc.kind = RK_REG;
        rc.idx = (uint8_t)(code - 4);
    } else if (code == 30) {
        rc.kind = RK_PCSH;
    }
    return rc;
}

/*
 * Operand read.  pc reads the address of the instruction after the
 * latent instruction, i.e. insn_addr + 8 [IM CALL page; DOC §1.5.4].
 * The -n/+n pseudo-operands read as -1/+1 in ALU source positions
 * (INCR/DECR pages); the sp-bump ±4 special case is handled by the ALU.
 * Reserved codes read as 0 (hardware behaviour undocumented).
 */
static uint32_t opval(dsp3210_t *s, unsigned code) {
    regcode rc = rc_decode(code);
    switch (rc.kind) {
    case RK_REG:
        return s->r[rc.idx];
    case RK_PC:
        return s->cur_insn + 8;
    case RK_PCSH:
        return s->pcsh;
    case RK_PLUS:
        return 1;
    case RK_MINUS:
        return 0xFFFFFFFFu;
    default:
        return 0; /* r0 / reserved */
    }
}

/*
 * Register write.  r0, the pseudo-operands and reserved codes discard.
 * pcsh is writable (the boot ROM's SAR path depends on it).  A write to
 * pc is not in any replacement table; we implement it as an immediate
 * branch (documented deviation for an undefined encoding).
 */
static void regw(dsp3210_t *s, unsigned code, uint32_t v) {
    regcode rc = rc_decode(code);
    switch (rc.kind) {
    case RK_REG:
        s->r[rc.idx] = v;
        break;
    case RK_PC:
        s->npc = v;
        break;
    case RK_PCSH:
        s->pcsh = v;
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* exceptions [IM §7.5]                                                */

static int raw_access(dsp3210_t *s, uint32_t addr, int size, uint32_t *inout, int write);
static int safe_read(dsp3210_t *s, uint32_t addr, int size, uint32_t *out);

/* Raise an exception.  Returns 1 if the exception was actually taken
 * (the current instruction must abort), 0 if it was masked or ignored
 * (the instruction proceeds — see the address-error note in mem_read). */
static int take_error(dsp3210_t *s, int vn) {
    s->last_vector = vn;
    if (vn >= 4 && !(s->emr & (1u << vn)))
        return 0; /* masked → no exception */
    if (s->level == DSP3210_LVL_DERROR)
        return 1;
    if (s->level == DSP3210_LVL_ERROR) {
        if (vn >= 4)
            return 0; /* maskable at error level:
                         ignored [IM §7.5.2] */
        if (s->error_nonmaskable) { /* non-maskable during
                                       non-maskable → double */
            s->level = DSP3210_LVL_DERROR;
            return 1;
        }
    }
    /* abort model: kill do loops, disable interrupts, unlock pcw
     * [IM §7.5.2, Table 7-3] */
    s->do_active = 0;
    s->waiting = 0;
    s->error_nonmaskable = (vn < 4);
    s->level = DSP3210_LVL_ERROR;
    s->pcw &= (uint16_t) ~(1u << 13);
    s->pcw_locked = 0;
    /* dispatch = call evtp + vn*8 (r20); r20 gets the pc, which is the
     * address of the prefetched instruction (= aborted insn + 8, the
     * same value pc reads as an operand) */
    s->r[20] = s->cur_insn + 8;
    s->pc = s->r[22] + (uint32_t)vn * 8;
    s->npc = s->pc + 4;
    return 1;
}

static int pending_vector(dsp3210_t *s) {
    unsigned p = s->pending & s->emr & 0xFF00u;
    int vn;
    if (!p)
        return 0;
    for (vn = 8; vn <= 15; vn++) /* EXT0 (8) highest priority */
        if (p & (1u << vn))
            return vn;
    return 0;
}

/* PS.IR0/IR1 read the LIVE pin level, 1 = negated: the board's frame tick
 * is a short active-low pulse, and the RTM kernel's `if (ir1s)` spins wait
 * for the pulse both in the frame-period calibration gadget and in the
 * per-module overrun poll (dsp-kernel-messages.md §3.2/§3.4).  A latched
 * mirror here makes both spins fall through instantly and the kernel
 * measures a ~0-tick frame period — killing the GPB admission check. */
static void update_irq_ps(dsp3210_t *s) {
    s->ps = (uint16_t)((s->ps & ~(DSP3210_PS_IR0 | DSP3210_PS_IR1)) | (s->ext_pulse[0] ? 0 : DSP3210_PS_IR0) |
                       (s->ext_pulse[1] ? 0 : DSP3210_PS_IR1));
}

/* Burn one instruction-slot of core time off the live pin-pulse windows. */
static void ext_pulse_tick(dsp3210_t *s) {
    if (s->ext_pulse[0] && --s->ext_pulse[0] == 0)
        update_irq_ps(s);
    if (s->ext_pulse[1] && --s->ext_pulse[1] == 0)
        update_irq_ps(s);
}

static void take_interrupt(dsp3210_t *s, int vn) {
    s->last_vector = vn;
    s->pending &= (uint16_t) ~(1u << vn);
    update_irq_ps(s); /* before ps is shadowed */
    /* hardware context save [IM §7.5.1]: a0-a3 + dauc (+ ps/ctr per
     * Figure 4-1) + invisible do-loop state; resume address in pcsh */
    s->sh_ps = s->ps;
    s->sh_dauc = s->dauc;
    s->sh_ctr = s->ctr;
    memcpy(s->sh_a, s->a, sizeof s->a);
    memcpy(s->sh_a_pipe, s->a_pipe, sizeof s->a_pipe);
    memcpy(s->sh_dau_flag_pipe, s->dau_flag_pipe, sizeof s->dau_flag_pipe);
    s->sh_do_active = s->do_active;
    s->sh_do_lock = s->do_lock;
    s->sh_do_start = s->do_start;
    s->sh_do_end = s->do_end;
    s->sh_do_count = s->do_count;
    s->do_active = 0;
    /* the not-yet-executed instruction at the resume address goes into
     * the instruction shadow register — as prefetched, i.e. under the
     * memory map in force while the PREVIOUS instruction executed (the
     * boot ROM's SAR flow prefetches `*r0 = r0` in computer mode and
     * replays it in processor mode) — and pcsh points at the location
     * to be fetched when exiting [IM §7.5.1, IRETURN page] */
    if (s->prefetch_addr == s->pc) {
        s->irsh = s->prefetch;
    } else {
        s->irsh = 0x80000000u; /* nop */
        safe_read(s, s->pc, 4, &s->irsh);
    }
    s->irsh_addr = s->pc;
    s->pcsh = s->pc + 4;
    s->level = DSP3210_LVL_INTERRUPT;
    s->pc = s->r[22] + (uint32_t)vn * 8; /* goto evtp + vn*8 ; nop */
    s->npc = s->pc + 4;
}

static void do_ireturn(dsp3210_t *s) {
    if (s->level == DSP3210_LVL_INTERRUPT) {
        s->ps = s->sh_ps;
        s->dauc = s->sh_dauc;
        s->ctr = s->sh_ctr;
        memcpy(s->a, s->sh_a, sizeof s->a);
        memcpy(s->a_pipe, s->sh_a_pipe, sizeof s->a_pipe);
        memcpy(s->dau_flag_pipe, s->sh_dau_flag_pipe, sizeof s->dau_flag_pipe);
        s->do_active = s->sh_do_active;
        s->do_lock = s->sh_do_lock;
        s->do_start = s->sh_do_start;
        s->do_end = s->sh_do_end;
        s->do_count = s->sh_do_count;
        s->level = DSP3210_LVL_BASE;
    }
    /* No latent instruction: the word after ireturn is never executed.
     * Instead the instruction prefetched before the interrupt executes
     * from the instruction shadow register, and fetching resumes at
     * pcsh [IM IRETURN page, Figure 7-6].  The boot ROM's SAR path
     * depends on the replay: its EXT1 handler overwrites pcsh with the
     * host vector and lets the replayed `*r0 = r0` post the boot-done
     * signal. */
    s->pc = s->pcsh;
    s->npc = s->pcsh + 4;
    s->irsh_pending = 1;
    update_irq_ps(s); /* re-derive after ps restore */
}

/* ------------------------------------------------------------------ */
/* on-chip timer + BIO [IM §9.3, §9.4; DOC §1.4]                       */

/* MMIO word offsets inside the on-chip window (aligned big-endian slots;
 * the narrow registers live in the LOW lanes of their word — which is why
 * the kernel's masked-address-error (long) store to $413 lands on tcon). */
#define DSP_MMIO_BASE  0x0400u
#define DSP_MMIO_END   0x0800u
#define DSP_MMIO_TCON  0x0410u /* tcon byte at $0413 (low lane) */
#define DSP_MMIO_TIMER 0x0414u
#define DSP_MMIO_BIOC  0x0418u /* bioc byte at $041B (low lane) */
#define DSP_MMIO_BIO   0x041Cu /* bio half-word at $041E (low lanes) */

/* Timer ticks per instruction cycle: SRC 000 = CKI/2 (2 per instruction at
 * 4 CKI each), 001 = CKI/4 (1 per instruction); the BIO0 external sources
 * never tick here (no external clock exists in the emulator). */
static int timer_ticks_per_insn(const dsp3210_t *s) {
    if (!(s->tcon & 1))
        return 0; /* E/HN = 0: count held */
    switch ((s->tcon >> 5) & 7) {
    case 0:
        return 2;
    case 1:
        return 1;
    default:
        return 0;
    }
}

/* Advance the down-counter.  Reaching zero always raises the TIMER request
 * (taken only if emr[9]); tcon[1] auto-reloads from the initial-count
 * register — reload period = N+1 ticks [IM §9.3.2]. */
static void timer_tick(dsp3210_t *s, int ticks) {
    while (ticks-- > 0) {
        if (s->timer_count == 0) {
            if (!(s->tcon & 2))
                return; /* R/SN = 0: stop at zero */
            s->timer_count = s->timer_reload; /* the reload consumes a tick */
            continue;
        }
        if (--s->timer_count == 0)
            s->pending |= (uint16_t)(1u << DSP3210_VEC_TIMER);
    }
}

/* Pin levels the board can observe.  The output REGISTER is reported
 * regardless of bioc: the AV board's doorbell latch follows the kernel's
 * per-message BIO0 toggle without the kernel ever programming bioc
 * (gap-closure B1: "any BIO0 output transition"). */
static uint8_t bio_pins(const dsp3210_t *s) {
    return s->bio_out;
}

/* bio 16-bit write: eight 2-bit fields BFn at bits 2n+1:2n — 00 hold,
 * 01 clear, 10 set, 11 complement the output-register bit [IM §9.4].
 * Output transitions surface through the board callback (the AV PSC
 * latches L5 bit 0 on any BIO0 transition — the RTM message doorbell). */
static void bio_write(dsp3210_t *s, uint16_t v) {
    uint8_t old_pins = bio_pins(s);
    int n;
    for (n = 0; n < 8; n++) {
        switch ((v >> (2 * n)) & 3) {
        case 1:
            s->bio_out &= (uint8_t) ~(1u << n);
            break;
        case 2:
            s->bio_out |= (uint8_t)(1u << n);
            break;
        case 3:
            s->bio_out ^= (uint8_t)(1u << n);
            break;
        default:
            break;
        }
    }
    if (s->bio_fn && bio_pins(s) != old_pins)
        s->bio_fn(s->bio_ctx, old_pins, bio_pins(s));
}

/* bioc: direction bits, 1 = output; re-driving a pin is a transition too */
static void bioc_write(dsp3210_t *s, uint8_t v) {
    uint8_t old_pins = bio_pins(s);
    s->bioc = v;
    if (s->bio_fn && bio_pins(s) != old_pins)
        s->bio_fn(s->bio_ctx, old_pins, bio_pins(s));
}

/* bio 16-bit read: bit 2n = pin level (inputs read 0 here), bit 2n+1 =
 * the output-register value [IM §9.4]. */
static uint16_t bio_read(const dsp3210_t *s) {
    uint16_t v = 0;
    int n;
    for (n = 0; n < 8; n++) {
        unsigned out = (s->bio_out >> n) & 1;
        unsigned pin = ((s->bioc >> n) & 1) ? out : 0;
        v |= (uint16_t)((pin | (out << 1)) << (2 * n));
    }
    return v;
}

/* MMIO access inside the on-chip window.  Only the timer and BIO decode;
 * SIO/DMAC words fall through to the RAM shadow (present-but-inert).
 * Returns 1 when the access was handled. */
static int mmio_read(dsp3210_t *s, uint32_t off, int size, uint32_t *out) {
    if (off == DSP_MMIO_TIMER && size == 4) {
        *out = s->timer_count;
        return 1;
    }
    if ((off == 0x0413u && size == 1) || (off == DSP_MMIO_TCON && size == 4)) {
        *out = s->tcon;
        return 1;
    }
    if ((off == 0x041Bu && size == 1) || (off == DSP_MMIO_BIOC && size == 4)) {
        *out = s->bioc;
        return 1;
    }
    if ((off == 0x041Eu && size == 2) || (off == DSP_MMIO_BIO && size == 4)) {
        *out = bio_read(s);
        return 1;
    }
    return 0;
}

static int mmio_write(dsp3210_t *s, uint32_t off, uint32_t val, int size) {
    if (off == DSP_MMIO_TIMER && size == 4) {
        s->timer_count = val; /* loads counter AND reload */
        s->timer_reload = val;
        return 1;
    }
    if ((off == 0x0413u && size == 1) || (off == DSP_MMIO_TCON && size == 4)) {
        s->tcon = (uint8_t)val; /* low byte lane */
        return 1;
    }
    if ((off == 0x041Bu && size == 1) || (off == DSP_MMIO_BIOC && size == 4)) {
        bioc_write(s, (uint8_t)val);
        return 1;
    }
    if ((off == 0x041Eu && size == 2) || (off == DSP_MMIO_BIO && size == 4)) {
        bio_write(s, (uint16_t)val);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* memory [DOC §1.1]                                                   */

/* On-chip window offset for addr, or -1 when the access is external.
 * The window is 64 KB (boot ROM, MMIO, RAM) decoded at $0000xxxx in
 * computer mode (pcw[10]=1) and at $5003xxxx in processor mode [IM
 * Figure 3-4].  Accesses just past the window ($50040000+) are BOARD
 * targets, not on-chip: on the AV they hit the Singer sound-FIFO ring
 * window (see the AV glue's bus hooks). */
static int32_t chip_window_off(const dsp3210_t *s, uint32_t addr) {
    uint32_t base = (s->pcw & (1u << 10)) ? 0u : 0x50030000u;
    uint32_t off = addr - base;
    if (off < 0x10000u)
        return (int32_t)off;
    return -1;
}

static uint8_t *mem_ptr(dsp3210_t *s, uint32_t addr) {
    /* On-chip window first (incl. the RAM mirror); external memory A
     * backs everything else (it starts at $10000 in computer mode). */
    int32_t off = chip_window_off(s, addr);
    if (off >= 0)
        return &s->chip[off];
    if (addr < s->mem_size)
        return &s->mem[addr];
    return NULL;
}

/* raw access, no exceptions — for loaders and hosts */
static int raw_access(dsp3210_t *s, uint32_t addr, int size, uint32_t *inout, int write) {
    int big = (s->pcw >> 8) & 1, i;
    uint32_t v = write ? *inout : 0;
    if (addr & (uint32_t)(size - 1))
        return -1;
    for (i = 0; i < size; i++) {
        int shift = 8 * (big ? size - 1 - i : i);
        uint8_t *p = mem_ptr(s, addr + (uint32_t)i);
        if (!p)
            return -1;
        if (write)
            *p = (uint8_t)(v >> shift);
        else
            v |= (uint32_t)*p << shift;
    }
    if (!write)
        *inout = v;
    return 0;
}

/* ---- the DA store shadow [IM §4.4.2.1] ---- */

static uint32_t byte_lane(uint32_t v, int size, int idx, int big) {
    return (v >> (8 * (big ? size - 1 - idx : idx))) & 0xFFu;
}

static uint32_t put_lane(uint32_t v, int size, int idx, int big, uint32_t b) {
    int sh = 8 * (big ? size - 1 - idx : idx);
    return (v & ~(0xFFu << sh)) | (b << sh);
}

/* Record the pre-write bytes of a DA store, so reads inside its three-
 * instruction shadow still see them. */
static void da_store_shadow(dsp3210_t *s, uint32_t addr, int size) {
    uint32_t i = s->da_wr_n & 3, old = 0;
    if (safe_read(s, addr, size, &old))
        return; /* unreadable: nothing to shadow */
    s->da_wr[i].addr = addr;
    s->da_wr[i].size = (uint8_t)size;
    s->da_wr[i].old = old;
    s->da_wr[i].ic = s->icount;
    s->da_wr_n++;
}

/* Overlay any byte still inside a DA store's shadow with its pre-write
 * value.  Oldest entry first, so the earliest pending write wins a byte
 * two of them touch — that is the content before the whole window. */
static void da_store_bypass(dsp3210_t *s, uint32_t addr, int size, uint32_t *val) {
    int big = (s->pcw >> 8) & 1, i, k;
    uint8_t done[4] = {0, 0, 0, 0};
    for (k = 4; k >= 1; k--) {
        uint32_t idx = (s->da_wr_n - (uint32_t)k) & 3;
        if (!s->da_wr[idx].size || s->icount - s->da_wr[idx].ic > 3)
            continue;
        for (i = 0; i < size; i++) {
            uint32_t a = addr + (uint32_t)i;
            if (done[i] || a < s->da_wr[idx].addr || a - s->da_wr[idx].addr >= s->da_wr[idx].size)
                continue;
            *val = put_lane(*val, size, i, big,
                            byte_lane(s->da_wr[idx].old, s->da_wr[idx].size, (int)(a - s->da_wr[idx].addr), big));
            done[i] = 1;
        }
    }
}

static int mem_read_raw(dsp3210_t *s, uint32_t addr, int size, uint32_t *out);

/* access from executing code — raises address/bus errors, and serves any
 * byte still inside a DA store's three-instruction shadow from `da_wr`
 * (instruction fetch is exempt: the kernel's chunk loader writes code and
 * the modules run from it much later). */
static int mem_read(dsp3210_t *s, uint32_t addr, int size, uint32_t *out) {
    int r = mem_read_raw(s, addr, size, out);
    if (!r && !s->in_fetch)
        da_store_bypass(s, addr & ~(uint32_t)(size - 1), size, out);
    return r;
}

/* A misaligned access raises the address error (vector 4).  When that
 * error is MASKED, the access still completes with the low address bits
 * ignored (aligned down): the AV ROM's kernel relies on this — it
 * programs the 8-bit tcon register with `*r7 = (long) r8` at $50030413
 * (kernel main, core image +$74 into the $1160 chunk), which only works
 * if the masked misaligned long store lands on the $0410 word slot with
 * tcon in its low byte lane. */
static int mem_read_raw(dsp3210_t *s, uint32_t addr, int size, uint32_t *out) {
    int fault = 0;
    uint32_t base = (s->pcw & (1u << 10)) ? 0u : 0x50030000u;
    if (addr & (uint32_t)(size - 1)) { /* [IM §7.5, address error] */
        if (take_error(s, DSP3210_VEC_AERR))
            return -1;
        addr &= ~(uint32_t)(size - 1);
    }
    /* the on-chip window decodes inside the core, before any hook
     * (cores.md contract): timer/BIO MMIO first, chip RAM otherwise
     * (incl. the RAM mirror above the window) */
    if (chip_window_off(s, addr) >= 0) {
        uint32_t off = addr - base;
        if (off - DSP_MMIO_BASE < DSP_MMIO_END - DSP_MMIO_BASE && mmio_read(s, off, size, out))
            return 0;
        if (raw_access(s, addr, size, out, 0)) {
            take_error(s, DSP3210_VEC_BUSERR);
            return -1;
        }
        return 0;
    }
    if (s->read_fn) {
        *out = s->read_fn(s->hook_ctx, addr, size, &fault);
        if (fault) {
            take_error(s, DSP3210_VEC_BUSERR);
            return -1;
        }
        return 0;
    }
    if (raw_access(s, addr, size, out, 0)) {
        take_error(s, DSP3210_VEC_BUSERR);
        return -1;
    }
    return 0;
}

static int mem_write(dsp3210_t *s, uint32_t addr, int size, uint32_t val) {
    int fault = 0;
    uint32_t base = (s->pcw & (1u << 10)) ? 0u : 0x50030000u;
    if (addr & (uint32_t)(size - 1)) {
        if (take_error(s, DSP3210_VEC_AERR))
            return -1;
        addr &= ~(uint32_t)(size - 1); /* see mem_read */
    }
    if (chip_window_off(s, addr) >= 0) {
        uint32_t off = addr - base;
        if (off - DSP_MMIO_BASE < DSP_MMIO_END - DSP_MMIO_BASE && mmio_write(s, off, val, size))
            return 0;
        if (raw_access(s, addr, size, &val, 1)) {
            take_error(s, DSP3210_VEC_BUSERR);
            return -1;
        }
        return 0;
    }
    if (s->write_fn) {
        s->write_fn(s->hook_ctx, addr, val, size, &fault);
        if (fault) {
            take_error(s, DSP3210_VEC_BUSERR);
            return -1;
        }
        return 0;
    }
    if (raw_access(s, addr, size, &val, 1)) {
        take_error(s, DSP3210_VEC_BUSERR);
        return -1;
    }
    return 0;
}

/* No-exception read that honours the injected hooks — the prefetch/irsh
 * model must see external memory when the core runs on a real bus (the
 * reference rigs fell through to flat memory; the AV glue has none). */
static int safe_read(dsp3210_t *s, uint32_t addr, int size, uint32_t *out) {
    if (addr & (uint32_t)(size - 1))
        return -1;
    if (chip_window_off(s, addr) < 0 && s->read_fn) {
        int fault = 0;
        *out = s->read_fn(s->hook_ctx, addr, size, &fault);
        return fault ? -1 : 0;
    }
    return raw_access(s, addr, size, out, 0);
}

/* ------------------------------------------------------------------ */
/* DSP32 floating point — exact integer model                          */
/*
 * 32-bit word = mantissa[31:8] | exponent[7:0]; the mantissa is a single
 * 2's-complement quantity s(!s).f…f, so M in [1,2) or [-2,-1), and
 * value = M * 2^(e-128).  e = 0 is zero (incl. "dirty zeros").
 * 40-bit accumulator = mantissa[39:16] | guard[15:8] | exponent[7:0],
 * i.e. the same mantissa with 8 extra fraction bits [IM Fig 8-8].
 *
 * dsp3210_acc.m holds the mantissa *including the implicit bit* as a
 * fixed-point integer with 31 fractional bits (24 mantissa + 8 guard - 1
 * for the implicit bit's integer position), so
 *      value = m * 2^(e-128) / 2^31.
 *
 * Internally the multiplier's product is carried at 46 fractional bits
 * (23+23, exact for a 25x25-bit signed multiply) and the adder aligns,
 * adds and truncates at that width before the result is truncated back
 * to the accumulator's 31 fractional bits [IM §8.2.3: "three integer
 * bits are retained in P", "the adder inputs and result contain eight
 * mantissa guard bits"].
 */

#define SC_ACC  31 /* accumulator fraction bits */
#define SC_MUL  46 /* product fraction bits */
#define ACC_ONE (INT64_C(1) << SC_ACC) /* |m| for M = 1 */

/*
 * Left-shift a signed 64-bit value by k.  Shifting a negative signed
 * value left is undefined in C99, and this file claims to be portable
 * C99, so route it through the unsigned type: the two's-complement bit
 * pattern is what the datapath shifts.  (The arithmetic RIGHT shifts
 * elsewhere are implementation-defined rather than undefined, and are
 * deliberate — they are the adder's truncation.)
 */
static int64_t shl64(int64_t v, int k) {
    return (int64_t)((uint64_t)v << k);
}

static dsp3210_acc acc_zero(void) {
    dsp3210_acc r;
    r.m = 0;
    r.e = 0;
    return r;
}

/*
 * Normalise so that M is in [1,2) or [-2,-1), i.e. at fraction scale
 * `sc`: m in [2^sc, 2^(sc+1)) for positive, [-2^(sc+1), -2^sc) for
 * negative.  Note -2^sc itself (M = -1) is NOT normal — it renormalises
 * to -2^(sc+1) with e-1, which is why -1.0 encodes as -2 * 2^-1.
 * Right shifts truncate (arithmetic), which is what the hardware does.
 */
static void norm_at(int64_t *m, int *e, int sc) {
    int64_t lo = INT64_C(1) << sc, hi = lo << 1;
    if (*m == 0) {
        *e = 0;
        return;
    }
    while (*m >= hi || *m < -hi) {
        *m >>= 1;
        (*e)++;
    }
    while ((*m > 0 && *m < lo) || (*m < 0 && *m >= -lo)) {
        *m = shl64(*m, 1);
        (*e)--;
    }
}

/* The 24-bit stored mantissa field of a 32-bit word, as the full
 * 2's-complement value including the implicit bit (23 fraction bits). */
static int64_t mant24_full(uint32_t w) {
    int32_t raw = (int32_t)(w & 0xFFFFFF00u) >> 8; /* signed 24-bit */
    return (int64_t)raw + (raw < 0 ? -(INT64_C(1) << 23) : (INT64_C(1) << 23));
}

dsp3210_acc dsp3210_acc_unpack(uint32_t w) {
    dsp3210_acc r;
    if ((w & 0xFFu) == 0)
        return acc_zero(); /* incl. dirty zeros */
    r.m = shl64(mant24_full(w), 8); /* 23 -> 31 fraction bits */
    r.e = (int16_t)(w & 0xFFu);
    return r;
}

uint32_t dsp3210_acc_pack(dsp3210_acc a) {
    int64_t m24;
    uint32_t raw;
    if (a.e == 0 || a.m == 0)
        return 0;
    m24 = a.m >> 8; /* truncate the guard bits */
    raw = (uint32_t)(m24 - (m24 < 0 ? -(INT64_C(1) << 23) : (INT64_C(1) << 23)));
    return ((raw & 0xFFFFFFu) << 8) | (uint32_t)(a.e & 0xFF);
}

/* The stored mantissa+guard field (accumulator bits 39-8): the value
 * with the implicit bit removed. */
static void dsp3210_acc_raw_of(dsp3210_acc a, int64_t *mant_guard) {
    *mant_guard = (a.e == 0) ? 0 : (a.m - (a.m < 0 ? -(INT64_C(1) << SC_ACC) : (INT64_C(1) << SC_ACC))) & 0xFFFFFFFFu;
}

double dsp3210_acc_double(dsp3210_acc a) {
    if (a.e == 0)
        return 0.0;
    return ldexp((double)a.m, (int)a.e - 128 - SC_ACC);
}

dsp3210_acc dsp3210_acc_from_double(double v) {
    dsp3210_acc r;
    int exp;
    double f;
    if (v == 0.0 || !isfinite(v))
        return acc_zero();
    /* A host double stands in for a 32-bit DSP32 datum, so round to the
     * 24-bit mantissa and leave the guard bits zero — exactly what
     * loading a memory operand into an accumulator gives. */
    f = frexp(v, &exp); /* v = f * 2^exp, |f|∈[.5,1) */
    {
        int e = exp - 1 + 128;
        /* Ties go to the greater value (toward +inf), matching the DSP's
         * own `round` instruction and the sibling core's helper, so both
         * cores encode host test data bit-identically [ERRATA B8]. */
        int64_t m = (int64_t)floor(f * ldexp(1.0, 24) + 0.5); /* scale 23 */
        norm_at(&m, &e, 23);
        if (e > 255) {
            r.m = (v < 0) ? -(INT64_C(1) << 32) : (INT64_C(1) << 32) - 256;
            r.e = 255;
            return r;
        }
        if (e < 1)
            return acc_zero();
        r.m = shl64(m, 8); /* scale 23 -> 31, guard = 0 */
        r.e = (int16_t)e;
    }
    return r;
}

/* legacy helpers, kept so rigs and tests can build against either core */
double dsp3210_dsp32_to_double(uint32_t w) {
    return dsp3210_acc_double(dsp3210_acc_unpack(w));
}

uint32_t dsp3210_double_to_dsp32(double v) {
    if (v != 0.0 && isfinite(v)) {
        int exp;
        frexp(v, &exp);
        if (exp - 1 + 128 > 255) /* saturate [IM §8.2.4.1] */
            return v > 0 ? 0x7FFFFFFFu : 0x800000FFu;
    }
    return dsp3210_acc_pack(dsp3210_acc_from_double(v));
}

/* ---- the multiplier: exact 25x25 -> 50-bit signed product ---- */
/* Inputs are 32-bit words (an accumulator feeding the multiplier has
 * already had its guard bits truncated by acc_pack) [IM §8.2.3]. */
static void dau_mul(uint32_t xw, uint32_t yw, int64_t *pm, int *pe) {
    unsigned ex = xw & 0xFFu, ey = yw & 0xFFu;
    if (ex == 0 || ey == 0) {
        *pm = 0;
        *pe = 0;
        return;
    }
    *pm = mant24_full(xw) * mant24_full(yw); /* scale 46 */
    *pe = (int)ex + (int)ey - 128;
}

/* ---- the adder: S + P, exact align/add, truncate to 24+8 bits ---- */
static dsp3210_acc dau_add(dsp3210_acc s, int64_t pm, int pe) {
    int64_t sm, rm;
    int se, re;

    sm = (s.e == 0) ? 0 : shl64(s.m, SC_MUL - SC_ACC); /* 31 -> 46 bits */
    se = s.e;

    if (sm == 0 && pm == 0)
        return acc_zero();
    if (sm == 0) {
        rm = pm;
        re = pe;
    } else if (pm == 0) {
        rm = sm;
        re = se;
    } else if (se >= pe) {
        int d = se - pe;
        rm = sm + (d > 62 ? 0 : (pm >> d));
        re = se;
    } else {
        int d = pe - se;
        rm = pm + (d > 62 ? 0 : (sm >> d));
        re = pe;
    }
    norm_at(&rm, &re, SC_MUL);
    if (rm == 0)
        return acc_zero();
    {
        dsp3210_acc r;
        r.m = rm >> (SC_MUL - SC_ACC); /* truncate to 24+8 bits */
        r.e = (int16_t)re;
        {
            int e = re;
            norm_at(&r.m, &e, SC_ACC); /* no-op in practice */
            r.e = (int16_t)e;
        }
        return r;
    }
}

static dsp3210_acc acc_negate(dsp3210_acc a) {
    int e = a.e;
    if (a.e == 0)
        return a;
    a.m = -a.m;
    norm_at(&a.m, &e, SC_ACC); /* -(-2) = 2 needs e+1 */
    a.e = (int16_t)e;
    return a;
}

/* round(): 40-bit -> 32-bit, round to nearest, ties to the greater
 * value, guard bits cleared [IM ROUND page] */
static dsp3210_acc acc_round(dsp3210_acc a) {
    int e = a.e;
    if (a.e == 0)
        return a;
    a.m = shl64((a.m + 128) >> 8, 8); /* +2^7 then truncate */
    norm_at(&a.m, &e, SC_ACC);
    a.e = (int16_t)e;
    return a;
}

/* exact integer -> float */
static dsp3210_acc acc_from_i64(int64_t v) {
    dsp3210_acc r;
    int e = 128 + SC_ACC;
    if (v == 0)
        return acc_zero();
    r.m = v;
    norm_at(&r.m, &e, SC_ACC);
    r.e = (int16_t)e;
    return r;
}

/*
 * exact float -> integer, with the dauc[5:4] rounding modes and
 * saturation [IM §8.2.5, INT16/INT32 pages].  `bits` is 8, 16 or 32.
 */
static int64_t acc_to_int(dsp3210_acc a, unsigned dauc, int bits) {
    int mode = (dauc >> 4) & 3;
    int shift;
    int64_t i, frac_mask, lo, hi;

    if (bits == 8) {
        lo = 0;
        hi = 255;
    } else if (bits == 16) {
        lo = -32768;
        hi = 32767;
    } else {
        lo = INT32_MIN;
        hi = INT32_MAX;
    }

    if (a.e == 0)
        return 0;
    shift = (int)a.e - 128 - SC_ACC; /* value = m * 2^shift */
    if (shift >= 0) { /* integral; may overflow */
        if (shift > 40)
            return a.m < 0 ? lo : hi;
        i = shl64(a.m, shift > 24 ? 24 : shift);
        if (shift > 24)
            return a.m < 0 ? lo : hi;
    } else {
        int sh = -shift;
        if (sh > 63) {
            /* |value| < 2^-32: the whole mantissa shifts out.  The
             * rounding mode still decides the result — floor gives -1
             * for any negative, nearest and truncate-towards-zero give
             * 0 [IM §8.2.5] — so do NOT short-circuit past it. */
            i = (mode == 1 && a.m < 0) ? -1 : 0;
        } else {
            frac_mask = (INT64_C(1) << sh) - 1;
            i = a.m >> sh; /* floor */
            if (!(mode & 1)) { /* x0: round to nearest,
                                  ties up = floor(v + 1/2) */
                if ((a.m >> (sh - 1)) & 1)
                    i += 1;
            } else if (mode == 3) { /* truncate towards zero */
                if (a.m < 0 && (a.m & frac_mask))
                    i += 1;
            }
            /* mode 1 (truncate towards -inf) is plain floor */
        }
    }
    if (i < lo)
        return lo;
    if (i > hi)
        return hi;
    return i;
}

/* ---- IEEE 754 single <-> DSP32, exact, no host float involved ---- */
static uint32_t acc_to_ieee(dsp3210_acc a) {
    int64_t m = a.m;
    int e = (int)a.e - 128; /* value = M * 2^e */
    uint32_t sign = 0, frac;
    if (a.e == 0 || m == 0)
        return 0; /* DSP32 zero -> +0.0 */
    if (m < 0) {
        sign = 0x80000000u;
        m = -m; /* |M| in (1, 2] */
        if (m == (INT64_C(1) << 32)) {
            m >>= 1;
            e += 1;
        } /* |M| == 2 */
    }
    /* now m in [2^31, 2^32): 1.f with 31 fraction bits; IEEE keeps 23 */
    frac = (uint32_t)((m - (INT64_C(1) << 31)) >> 8); /* truncate */
    e += 127; /* IEEE bias: E = dspexp + 127 */
    if (e >= 255)
        return sign | 0x7F800000u; /* out of IEEE range */
    if (e <= 0)
        return sign; /* flush denormals to zero */
    return sign | ((uint32_t)e << 23) | (frac & 0x7FFFFFu);
}

static dsp3210_acc acc_from_ieee(uint32_t w) {
    unsigned sgn = w >> 31, exp = (w >> 23) & 0xFFu;
    uint32_t frac = w & 0x7FFFFFu;
    dsp3210_acc r;
    int e;
    if (exp == 0)
        return acc_zero(); /* zero and denormals */
    r.m = (INT64_C(1) << 31) | ((int64_t)frac << 8);
    e = (int)exp - 127 + 128; /* dspexp = E - 127 */
    if (sgn) {
        r.m = -r.m;
        norm_at(&r.m, &e, SC_ACC);
    }
    if (e > 255) {
        r.m = sgn ? -(INT64_C(1) << 32) : (INT64_C(1) << 32) - 1;
        e = 255;
    }
    if (e < 1)
        return acc_zero();
    r.e = (int16_t)e;
    return r;
}

/* ------------------------------------------------------------------ */
/* CAU flags (Table 4-1)                                               */

static void set_flags(dsp3210_t *s, unsigned n, unsigned z, unsigned v, unsigned c) {
    s->ps = (uint16_t)((s->ps & ~0xFu) | (n ? DSP3210_PS_n : 0) | (z ? DSP3210_PS_z : 0) | (v ? DSP3210_PS_v : 0) |
                       (c ? DSP3210_PS_c : 0));
}

/* add with carry-in; flags per 16/32-bit rules; result sign-extended
 * when short [ADD page] */
static uint32_t alu_add(dsp3210_t *s, uint32_t a, uint32_t b, unsigned cin, int w16, int setf) {
    if (w16) {
        uint32_t ua = a & 0xFFFFu, ub = b & 0xFFFFu;
        uint32_t sum = ua + ub + cin;
        uint32_t res = (uint32_t)sext16(sum);
        if (setf)
            set_flags(s, (res >> 31) & 1, (sum & 0xFFFFu) == 0, ((~(ua ^ ub) & (ua ^ sum)) >> 15) & 1, (sum >> 16) & 1);
        return res;
    } else {
        uint64_t sum = (uint64_t)a + b + cin;
        uint32_t res = (uint32_t)sum;
        if (setf)
            set_flags(s, res >> 31, res == 0, (uint32_t)((~(a ^ b) & (a ^ res)) >> 31) & 1, (unsigned)(sum >> 32) & 1);
        return res;
    }
}

/* a - b; c is the borrow (c=1 ⇒ borrow occurred) — matches the negate
 * special case documented on the SUBTRACT page */
static uint32_t alu_sub(dsp3210_t *s, uint32_t a, uint32_t b, int w16, int setf) {
    if (w16) {
        uint32_t ua = a & 0xFFFFu, ub = b & 0xFFFFu;
        uint32_t diff = ua - ub;
        uint32_t res = (uint32_t)sext16(diff);
        if (setf)
            set_flags(s, (res >> 31) & 1, (diff & 0xFFFFu) == 0, (((ua ^ ub) & (ua ^ diff)) >> 15) & 1, ua < ub);
        return res;
    } else {
        uint32_t res = a - b;
        if (setf)
            set_flags(s, res >> 31, res == 0, (((a ^ b) & (a ^ res)) >> 31) & 1, a < b);
        return res;
    }
}

/* logic-class flags: nz, v=c=0 */
static void flags_nz(dsp3210_t *s, uint32_t res, int w16) {
    if (w16)
        set_flags(s, (res >> 15) & 1, (res & 0xFFFFu) == 0, 0, 0);
    else
        set_flags(s, res >> 31, res == 0, 0, 0);
}

static uint32_t bitrev(uint32_t x, int nbits) {
    uint32_t r = 0;
    int i;
    for (i = 0; i < nbits; i++)
        r |= ((x >> i) & 1u) << (nbits - 1 - i);
    return r;
}

/* condition evaluation (Table 4-7 / DOC §1.5.2) */
#define DSP3210_PS_DAU (DSP3210_PS_N | DSP3210_PS_Z | DSP3210_PS_U | DSP3210_PS_V)

static int cond_eval(dsp3210_t *s, unsigned c) {
    unsigned ps = s->ps;
    unsigned n = !!(ps & DSP3210_PS_n), z = !!(ps & DSP3210_PS_z);
    unsigned v = !!(ps & DSP3210_PS_v), cf = !!(ps & DSP3210_PS_c);
    /* DAU conditions read the flag pipeline, not the live flags
     * [IM §4.4.2.4] — see the header.  CAU flags have no such latency. */
    unsigned dps = s->dau_flag_pipe[3];
    unsigned AN = !!(dps & DSP3210_PS_N), AZ = !!(dps & DSP3210_PS_Z);
    unsigned AU = !!(dps & DSP3210_PS_U), AV = !!(dps & DSP3210_PS_V);

    switch (c & 63) {
    case 0:
        return 0; /* false */
    case 1:
        return 1; /* true */
    case 2:
        return !n; /* pl */
    case 3:
        return n; /* mi */
    case 4:
        return !z; /* ne */
    case 5:
        return z; /* eq */
    case 6:
        return !v; /* vc */
    case 7:
        return v; /* vs */
    case 8:
        return !cf; /* cc */
    case 9:
        return cf; /* cs */
    case 10:
        return !(n ^ v); /* ge */
    case 11:
        return n ^ v; /* lt */
    case 12:
        return !(z | (n ^ v)); /* gt */
    case 13:
        return z | (n ^ v); /* le */
    case 14:
        return !(cf | z); /* hi (unsigned >) */
    case 15:
        return cf | z; /* ls (unsigned <=) */
    case 16:
        return !AU; /* auc */
    case 17:
        return AU; /* aus */
    case 18:
        return !AN; /* age */
    case 19:
        return AN; /* alt */
    case 20:
        return !AZ; /* ane */
    case 21:
        return AZ; /* aeq */
    case 22:
        return !AV; /* avc */
    case 23:
        return AV; /* avs */
    case 24:
        return !(AN | AZ); /* agt */
    case 25:
        return AN | AZ; /* ale */
    case 32:
        return !(ps & DSP3210_PS_IBF); /* ibe */
    case 33:
        return !!(ps & DSP3210_PS_IBF); /* ibf */
    case 34:
        return !(ps & DSP3210_PS_OBE); /* obf */
    case 35:
        return !!(ps & DSP3210_PS_OBE); /* obe */
    case 40:
        return !(ps & DSP3210_PS_SY); /* syc */
    case 41:
        return !!(ps & DSP3210_PS_SY); /* sys */
    case 42:
        return !(ps & DSP3210_PS_FB); /* fbc */
    case 43:
        return !!(ps & DSP3210_PS_FB); /* fbs */
    case 44:
        return !(ps & DSP3210_PS_IR0); /* ir0c */
    case 45:
        return !!(ps & DSP3210_PS_IR0); /* ir0s */
    case 46:
        return !(ps & DSP3210_PS_IR1); /* ir1c */
    case 47:
        return !!(ps & DSP3210_PS_IR1); /* ir1s */
    default:
        return 0; /* reserved: undefined, take false */
    }
}

/* ------------------------------------------------------------------ */
/* IO registers (format 7b/7d) [DOC §1.4]                              */

/* spc write side effects, distinguished only by the W size */
enum { SPC_NONE = 0, SPC_SFTRST, SPC_BKPT, SPC_WAITI };

static uint32_t ior_read(dsp3210_t *s, unsigned n) {
    switch (n & 31) {
    case 0:
        return s->ps & 0x3FFFu; /* bits 15-14 read 0 */
    case 8:
        return s->emr;
    case 10:
        return 0; /* spc is write-only */
    case 12:
        return s->pcw;
    case 14:
        return s->dauc;
    case 15:
        return s->ctr & 0x3Fu;
    default:
        return 0; /* reserved */
    }
}

static int ior_write(dsp3210_t *s, unsigned n, uint32_t v, int size) {
    switch (n & 31) {
    case 0: /* only CAU flags are writable */
        s->ps = (uint16_t)((s->ps & ~0xFu) | (v & 0xFu));
        break;
    case 8:
        s->emr = (uint16_t)v;
        /* emr bit 0 has no maskable source; the kernel pulses it to drop a
         * latched-but-untaken EXT1 edge without taking the interrupt
         * (dsp-kernel-messages.md §3.5 — gadget, slave entry, overrun) */
        if (v & 1u)
            s->pending &= (uint16_t) ~(1u << DSP3210_VEC_EXT1);
        break;
    case 10: /* spc pseudo-register */
        return size == 1 ? SPC_SFTRST : size == 2 ? SPC_BKPT : SPC_WAITI;
    case 12: /* pcw; bit 13 locks it */
        if (!s->pcw_locked) {
            s->pcw = (uint16_t)v;
            if (v & (1u << 13))
                s->pcw_locked = 1;
        }
        break;
    case 14:
        s->dauc = (uint8_t)(v & 0x7Fu); /* bit 7 must be 0 */
        break;
    case 15: /* ctr is read-only in practice */
    default:
        break;
    }
    return SPC_NONE;
}

/* ------------------------------------------------------------------ */
/* CA moves: sizes and extensions (LOAD/STORE pages)                   */

static int w_size(unsigned wsz) {
    switch (wsz & 7) {
    case 0:
    case 1:
    case 4:
        return 1; /* byte / char / hbyte */
    case 2:
    case 3:
        return 2; /* ushort / short */
    default:
        return 4; /* long (and reserved 101/110) */
    }
}

static uint32_t w_extend(unsigned wsz, uint32_t raw) {
    switch (wsz & 7) {
    case 0:
        return raw & 0xFFu; /* (byte) */
    case 1:
        return (uint32_t)(int32_t)(int8_t)raw; /* (char) */
    case 2:
        return raw & 0xFFFFu; /* (ushort) */
    case 3:
        return (uint32_t)sext16(raw); /* (short) */
    case 4:
        return (raw & 0xFFu) << 8; /* (hbyte) */
    default:
        return raw; /* (long) */
    }
}

static uint32_t w_select(unsigned wsz, uint32_t reg) {
    switch (wsz & 7) {
    case 0:
    case 1:
        return reg & 0xFFu; /* (byte)/(char): bits 7-0 */
    case 2:
    case 3:
        return reg & 0xFFFFu; /* 16-bit: bits 15-0 */
    case 4:
        return (reg >> 8) & 0xFFu; /* (hbyte): bits 15-8 */
    default:
        return reg;
    }
}

/* post-modification of rP by the rI code: r0 = +0, +n/-n = ±size,
 * anything else adds the register value (unscaled) */
static void ca_postmod(dsp3210_t *s, unsigned rp, unsigned ri, int size) {
    uint32_t delta;
    switch (ri & 31) {
    case 0:
        return;
    case 23:
        delta = (uint32_t)size;
        break;
    case 22:
        delta = (uint32_t)-size;
        break;
    default:
        delta = opval(s, ri);
        break;
    }
    regw(s, rp, opval(s, rp) + delta);
}

/* ------------------------------------------------------------------ */
/* DAU flags + clip-test history                                       */

/*
 * Flags and the clip-test history from an exact result.  Overflow and
 * underflow are judged against the 40-bit range: |value| <= (2-2^-31) *
 * 2^127 and >= 1 * 2^-127, i.e. the exponent leaving [1,255] after
 * normalisation [IM §8.2.1, DOC §1.5.6].
 */
static dsp3210_acc da_flags(dsp3210_t *s, dsp3210_acc res, int affect_vu) {
    unsigned N, Z, V = 0, U = 0;
    if (affect_vu && res.e != 0) {
        if (res.e > 255) {
            V = 1;
            res.m = res.m < 0 ? -(INT64_C(1) << 32) : (INT64_C(1) << 32) - 1;
            res.e = 255; /* saturate */
        } else if (res.e < 1) {
            U = 1;
            res = acc_zero(); /* flush to zero */
        }
    }
    N = res.e != 0 && res.m < 0;
    Z = res.e == 0;
    s->ps = (uint16_t)((s->ps & ~0xF0u) | (N ? DSP3210_PS_N : 0) | (Z ? DSP3210_PS_Z : 0) | (U ? DSP3210_PS_U : 0) |
                       (V ? DSP3210_PS_V : 0));
    s->ctr = (uint8_t)(((s->ctr << 1) | N) & 0x3F); /* [IM Table 8-4] */
    if (V || U)
        take_error(s, DSP3210_VEC_UV); /* masked unless emr[5] */
    return res;
}

/* ------------------------------------------------------------------ */
/* DA operands (7-bit X/Y/Z fields) [IM Table 10-3; DOC §1.5.3]        */

typedef struct {
    int from_acc; /* 1: accumulator (or reserved field) */
    unsigned acc; /* accumulator number when from_acc */
    uint32_t raw; /* raw bits read when memory */
    uint32_t maddr; /* effective address when memory (pre-post-modify) */
    unsigned preg; /* pointer register (r1-r14) when memory */
    dsp3210_acc val; /* the accumulator itself when from_acc */
} da_opnd;

static void da_postmod(dsp3210_t *s, unsigned p, unsigned i, int size) {
    switch (i & 7) {
    case 0:
        break;
    case 6:
        s->r[p] -= (uint32_t)size;
        break;
    case 7:
        s->r[p] += (uint32_t)size;
        break;
    default:
        s->r[p] += s->r[14 + (i & 7)];
        break; /* r15-r19, unscaled */
    }
}

/* Read an X/Y operand of the given size.  Returns -1 on a memory fault
 * (exception already raised).  Memory words are NOT interpreted here —
 * the caller converts .raw as float/int/byte as the instruction needs;
 * .val is filled with the accumulator value for register-direct fields. */
static int da_read(dsp3210_t *s, unsigned f, int size, da_opnd *o) {
    unsigned p = (f >> 3) & 15, i = f & 7;
    memset(o, 0, sizeof *o);
    if (p == 0 || p == 15) { /* register direct (or reserved) */
        o->from_acc = 1;
        o->acc = i & 3;
        o->val = s->a[i & 3]; /* the ADDER lane: current, full 40 bits */
        /* the MULTIPLIER lane: guard bits truncated [IM §8.2.3] AND three
         * instructions stale [IM §4.4.2.2] */
        o->raw = dsp3210_acc_pack(s->a_pipe[2][i & 3]);
        return 0;
    }
    o->maddr = s->r[p];
    o->preg = p;
    if (mem_read(s, s->r[p], size, &o->raw))
        return -1;
    da_postmod(s, p, i, size);
    return 0;
}

/*
 * Z with p=1111: undocumented "through Y" spelling ([IM] calls p=1111
 * not allowed; Apple's assembler emits it throughout the AV sound
 * modules).  The result stores through the Y operand's own effective
 * address, and Z's I field post-modifies Y's *pointer register* — e.g.
 * Midput's whole body is `a0 = *r2 * a1` with Z=$7F, an in-place gain
 * multiply that advances r2, and AppleSRC converts integers in place
 * with `a0 = float32(*r1)` Z=$78.  When Y is an accumulator there is
 * nothing to store through, which is where the old "p=1111,i=111 means
 * no write" reading came from.  Returns -1 on a write fault.
 */
static int da_store_z_through_y(dsp3210_t *s, unsigned z, int size, uint32_t val, const da_opnd *y) {
    unsigned i = z & 7;
    if (y->from_acc)
        return 0; /* no memory operand to store through */
    da_store_shadow(s, y->maddr, size);
    if (mem_write(s, y->maddr, size, val))
        return -1;
    da_postmod(s, y->preg, i, size);
    return 0;
}

/* "No write" Z spellings: manual uses p=0000,i=111; Apple's assembler
 * uses p=1111,i=111 — both accepted (README of dsp3210dis). */
static int da_z_is_write(unsigned z) {
    unsigned p = (z >> 3) & 15, i = z & 7;
    if (i == 7 && (p == 0 || p == 15))
        return 0;
    return p != 0;
}

static int da_write(dsp3210_t *s, unsigned f, int size, uint32_t val) {
    unsigned p = (f >> 3) & 15, i = f & 7;
    if (p == 0 || p == 15)
        return 0; /* no write */
    da_store_shadow(s, s->r[p], size);
    if (mem_write(s, s->r[p], size, val))
        return -1;
    da_postmod(s, p, i, size);
    return 0;
}

/* Perform a Z field's address post-modification WITHOUT storing.  The
 * ifalt/ifaeq/ifagt pages are explicit: with dauc[6] = 1 the memory
 * write is conditional "but the address postmodifications are always
 * performed". */
static void da_postmod_only(dsp3210_t *s, unsigned f, int size) {
    unsigned p = (f >> 3) & 15, i = f & 7;
    if (p == 0 || p == 15)
        return;
    da_postmod(s, p, i, size);
}

/* An operand feeding the ADDER keeps full 40-bit precision when it comes
 * from an accumulator; a memory operand has zero guard bits. */
static dsp3210_acc da_adder(const da_opnd *o) {
    return o->from_acc ? o->val : dsp3210_acc_unpack(o->raw);
}

/* An operand feeding the MULTIPLIER is always 32-bit: an accumulator's
 * guard bits are truncated first [IM §8.2.3]. */
static uint32_t da_mult(const da_opnd *o) {
    return o->raw;
}

/* ------------------------------------------------------------------ */
/* companded conversions (ic/oc) [IM §8.2.4.2]
 *
 * µ-law byte (bit 0 first): ~m0 ~m1 ~m2 ~m3 ~n0 ~n1 ~n2 ~s
 *   Y = (-1)^s * ((16.5+M)*2^N - 16.5)
 * A-law byte:               ~m0 m1 ~m2 m3 ~n0 n1 ~n2 ~s   (XOR 0xD5)
 *   Y = (-1)^s * (16.5+M)*2^N   (N>=1);   (-1)^s * (0.5+M)*2   (N=0)
 */

/* Exact integer decode.  The formulas produce half-integers for mu-law
 * ((16.5+M)*2^N - 16.5), so these return TWICE the value. */
static int64_t mulaw_decode_2x(unsigned b) {
    unsigned u = ~b & 0xFFu;
    unsigned M = u & 15, N = (u >> 4) & 7, S = (u >> 7) & 1;
    int64_t y = (int64_t)(33 + 2 * M) * ((int64_t)1 << N) - 33;
    return S ? -y : y;
}

static int64_t alaw_decode_2x(unsigned b) {
    unsigned u = (b ^ 0xD5u) & 0xFFu;
    unsigned M = u & 15, N = (u >> 4) & 7, S = (u >> 7) & 1;
    int64_t y = N ? (int64_t)(33 + 2 * M) * ((int64_t)1 << N) : (int64_t)(1 + 2 * M) * 2;
    return S ? -y : y;
}

/* Nearest-code encode, compared on the exact doubled integers so no host
 * float rounding enters.  256 candidates; performance is a non-goal. */
static int64_t acc_to_int(dsp3210_acc a, unsigned dauc, int bits);

static unsigned companded_encode_acc(dsp3210_acc v, int alaw) {
    dsp3210_acc t = v;
    int64_t v2;
    unsigned b, best = 0;
    int64_t bestd = INT64_MAX;
    if (t.e)
        t.e = (int16_t)(t.e + 1); /* * 2 exactly */
    v2 = acc_to_int(t, 0 /* round to nearest */, 32);
    for (b = 0; b < 256; b++) {
        int64_t d = (alaw ? alaw_decode_2x(b) : mulaw_decode_2x(b)) - v2;
        if (d < 0)
            d = -d;
        if (d < bestd) {
            bestd = d;
            best = b;
        }
    }
    return best;
}

/* ------------------------------------------------------------------ */
/* DA special functions (format 5) [IM Table 4-4]                      */

enum {
    G_IC = 0,
    G_OC = 1,
    G_FLOAT16 = 2,
    G_INT16 = 3,
    G_ROUND = 4,
    G_IFALT = 5,
    G_IFAEQ = 6,
    G_IFAGT = 7,
    G_FLOAT32 = 8,
    G_INT32 = 9,
    G_IEEE = 12,
    G_DSP = 13,
    G_SEED = 14
};

static int exec_da_special(dsp3210_t *s, uint32_t w) {
    unsigned g = bits(w, 26, 23);
    unsigned n = bits(w, 22, 21);
    unsigned y = bits(w, 13, 7);
    unsigned z = bits(w, 6, 0);
    int zw = da_z_is_write(z);
    int ysz = (g == G_IC) ? 1 : (g == G_FLOAT16) ? 2 : 4;
    int zsz = (g == G_OC) ? 1 : (g == G_INT16) ? 2 : 4;
    da_opnd Y;
    uint32_t zbits = 0;
    dsp3210_acc res = s->a[n];

    if (da_read(s, y, ysz, &Y))
        return DSP3210_STEP_OK;

    /* The special-function result reaches memory from the DAU's own S
     * register, which is why the manual requires the Z write to be
     * issued in the same instruction.  For the integer/companded/IEEE
     * conversions the accumulator's own bits are then "unpredictable";
     * this model leaves the numeric value there (documented deviation),
     * while the Z bits are exact. */
    switch (g) {
    case G_IC: { /* byte -> float, NZ00 */
        unsigned code = Y.from_acc ? (unsigned)((dsp3210_acc_pack(Y.val) >> 24) & 0xFFu) : (Y.raw & 0xFFu);
        unsigned conv = s->dauc & 15;
        if (conv & 4) { /* x1xx unsigned linear in */
            res = acc_from_i64((int64_t)code);
        } else { /* mu-law / A-law in: the
          decode is a half-integer, so build
          it from 2*value and halve exactly */
            int64_t twice = (conv & 1) ? alaw_decode_2x(code) : mulaw_decode_2x(code);
            res = acc_from_i64(twice);
            if (res.e)
                res.e = (int16_t)(res.e - 1);
        }
        res = da_flags(s, res, 0);
        zbits = dsp3210_acc_pack(res);
        break;
    }
    case G_OC: { /* float -> byte, no flags */
        unsigned conv = s->dauc & 15, code;
        if (conv & 8) { /* 1xxx unsigned linear out */
            int64_t t = acc_to_int(da_adder(&Y), s->dauc, 8);
            code = (unsigned)t;
        } else { /* mu-law / A-law out */
            code = companded_encode_acc(da_adder(&Y), (conv & 2) != 0);
        }
        res = acc_from_i64((int64_t)code);
        zbits = code;
        break;
    }
    case G_FLOAT16: { /* int16 -> float, NZ00 */
        int64_t iv =
            Y.from_acc ? (int64_t)(int16_t)(dsp3210_acc_pack(Y.val) >> 16) : (int64_t)(int16_t)(Y.raw & 0xFFFFu);
        res = da_flags(s, acc_from_i64(iv), 0);
        zbits = dsp3210_acc_pack(res);
        break;
    }
    case G_FLOAT32: { /* int32 -> float, NZ00 */
        /* An accumulator's integer lane is bits 39-8 — mantissa AND
         * guard bits, symmetric with where INT32 deposits its result —
         * not the packed mantissa||exponent word. */
        int64_t iv;
        if (Y.from_acc) {
            int64_t mg;
            dsp3210_acc_raw_of(Y.val, &mg);
            iv = (int64_t)(int32_t)(uint32_t)mg;
        } else {
            iv = (int64_t)(int32_t)Y.raw;
        }
        res = da_flags(s, acc_from_i64(iv), 0);
        zbits = dsp3210_acc_pack(res);
        break;
    }
    case G_INT16: { /* float -> int16, no flags */
        int64_t iv = acc_to_int(da_adder(&Y), s->dauc, 16);
        res = acc_from_i64(iv);
        zbits = (uint32_t)iv & 0xFFFFu;
        break;
    }
    case G_INT32: { /* float -> int32, no flags */
        int64_t iv = acc_to_int(da_adder(&Y), s->dauc, 32);
        res = acc_from_i64(iv);
        zbits = (uint32_t)iv;
        break;
    }
    case G_ROUND: { /* float40 -> float32, NZVU */
        res = da_flags(s, acc_round(da_adder(&Y)), 1);
        zbits = dsp3210_acc_pack(res);
        break;
    }
    case G_IFALT:
    case G_IFAEQ:
    case G_IFAGT: { /* conditional load */
        unsigned AN = !!(s->ps & DSP3210_PS_N), AZ = !!(s->ps & DSP3210_PS_Z);
        int cond = (g == G_IFALT) ? AN : (g == G_IFAEQ) ? AZ : !(AN | AZ);
        if (cond)
            res = da_adder(&Y);
        zbits = dsp3210_acc_pack(res);
        if (zw && (s->dauc & 0x40u) && !cond) {
            /* dauc[6] = 1: the store is conditional, but the pointer
             * post-modification still happens [IM IFALT/IFAEQ/IFAGT] */
            da_postmod_only(s, z, zsz);
            zw = 0;
        }
        break;
    }
    case G_IEEE: { /* DSP32 -> IEEE, no flags */
        dsp3210_acc yv = da_adder(&Y);
        zbits = acc_to_ieee(yv);
        res = yv;
        break;
    }
    case G_DSP: { /* IEEE -> DSP32, NZVU/NaN */
        uint32_t iw = Y.from_acc ? dsp3210_acc_pack(Y.val) : Y.raw;
        unsigned sgn = iw >> 31, exp = (iw >> 23) & 0xFFu;
        uint32_t mant = iw & 0x7FFFFFu;
        if (exp == 255) { /* +-inf / NaN [DSP page] */
            zbits = sgn ? 0x800000FFu : 0x7FFFFFFFu;
            res = dsp3210_acc_unpack(zbits);
            s->ps = (uint16_t)((s->ps & ~0xF0u) | (sgn ? DSP3210_PS_N : 0) | ((!sgn || mant) ? DSP3210_PS_V : 0));
            s->ctr = (uint8_t)(((s->ctr << 1) | (sgn ? 1 : 0)) & 0x3F);
            if (mant)
                take_error(s, DSP3210_VEC_NAN);
        } else if (exp == 0) { /* zero / denormal -> +0 */
            res = acc_zero();
            s->ps = (uint16_t)((s->ps & ~0xF0u) | DSP3210_PS_Z | (mant ? DSP3210_PS_U : 0));
            s->ctr = (uint8_t)((s->ctr << 1) & 0x3F);
            zbits = 0;
        } else {
            res = da_flags(s, acc_from_ieee(iw), 1);
            zbits = dsp3210_acc_pack(res);
        }
        break;
    }
    case G_SEED: { /* reciprocal seed, NZU V=0 */
        uint32_t yb = Y.from_acc ? dsp3210_acc_pack(Y.val) : Y.raw;
        uint32_t sb = yb ^ 0x7FFFFFFFu; /* invert all but the sign */
        /* SEED page: "DAU FLAGS AFFECTED: NZU, V=0" — inverting an
         * exponent of $FF yields 0, i.e. an underflow to zero */
        res = da_flags(s, dsp3210_acc_unpack(sb), 0);
        if ((yb & 0xFFu) == 0xFFu && (sb & 0xFFu) == 0)
            s->ps |= DSP3210_PS_U;
        zbits = sb;
        break;
    }
    default: /* reserved G codes */
        return DSP3210_STEP_OK;
    }

    s->a[n] = res;
    if (((z >> 3) & 15) == 15) {
        if (da_store_z_through_y(s, z, zsz, zbits, &Y))
            return DSP3210_STEP_OK;
    } else if (zw) {
        da_write(s, z, zsz, zbits);
    }
    return DSP3210_STEP_OK;
}

/* ------------------------------------------------------------------ */
/* DA multiply/accumulate (formats 1-4) [IM Table 4-2]                 */

static int exec_da(dsp3210_t *s, uint32_t w) {
    unsigned fmt = bits(w, 31, 29);
    unsigned m = bits(w, 28, 26);
    unsigned n = bits(w, 22, 21);
    unsigned xf = bits(w, 20, 14);
    unsigned yf = bits(w, 13, 7);
    unsigned zf = bits(w, 6, 0);
    unsigned fs = bits(w, 24, 24); /* sign before the adder operand */
    unsigned ss = bits(w, 23, 23); /* sign of the product term */
    int zw = da_z_is_write(zf);
    da_opnd X, Y;
    dsp3210_acc adder, res;
    int64_t pm;
    int pe, tap;

    static const uint32_t ONE = 0x00000080u; /* 1.0 as a DSP32 word */

    if (fmt == 3 && m >= 6)
        return exec_da_special(s, w);

    /* Operand fetch order is X then Y: the X and Y registers are loaded
     * in machine states 1 and 2 of the same instruction cycle
     * [IM §8.2.6, Figure 8-12].  It only matters when one pointer
     * register is post-modified in both fields (which the assembler
     * rejects), but there is no reason not to match the hardware. */
    if (da_read(s, xf, 4, &X))
        return DSP3210_STEP_OK;
    if (da_read(s, yf, 4, &Y))
        return DSP3210_STEP_OK;

    if (fmt == 1 && m == 6) {
        /* FADD-TAP: aN = [-](Z=Y) {+,-} X */
        adder = da_adder(&Y);
        dau_mul(ONE, da_mult(&X), &pm, &pe);
        tap = 1;
    } else if (fmt == 1) {
        /* FMULT-ADD-STORE family: aN = [-]Y {+,-} {aM,0,1} * X.
         * The adder input Y keeps its guard bits when it is an
         * accumulator; the multiplier inputs never do. */
        adder = da_adder(&Y);
        if (m == 4) {
            pm = 0;
            pe = 0;
        } /* 0.0 * X */
        else /* aM is a multiplier input: three instructions stale */
            dau_mul(m == 5 ? ONE : dsp3210_acc_pack(s->a_pipe[2][m]), da_mult(&X), &pm, &pe);
        tap = 0;
    } else {
        /* fmt 2 (tap) / fmt 3 (store): aN = [-]{aM,0,1} {+,-} Y*X */
        if (m == 4)
            adder = acc_zero();
        else if (m == 5)
            adder = dsp3210_acc_unpack(ONE);
        else
            adder = s->a[m];
        dau_mul(da_mult(&Y), da_mult(&X), &pm, &pe);
        tap = (fmt == 2);
    }

    if (fs)
        adder = acc_negate(adder);
    if (ss)
        pm = -pm; /* exact at scale 46 */

    res = da_flags(s, dau_add(adder, pm, pe), 1);
    s->a[n] = res;

    if (((zf >> 3) & 15) == 15) {
        uint32_t zbits = tap ? da_mult(&Y) : dsp3210_acc_pack(res);
        if (da_store_z_through_y(s, zf, 4, zbits, &Y))
            return DSP3210_STEP_OK;
    } else if (zw) {
        uint32_t zbits = tap ? da_mult(&Y) /* the tap passes Y on */
                             : dsp3210_acc_pack(res);
        da_write(s, zf, 4, zbits);
    }
    return DSP3210_STEP_OK;
}

/* ------------------------------------------------------------------ */
/* CA ALU (formats 6a-6d) [IM Table 10-2 "CA - F Field"]               */

enum {
    F_ADD = 0,
    F_SHL = 1,
    F_RSUB = 2,
    F_CRADD = 3,
    F_SUB = 4,
    F_RES5 = 5,
    F_ANDC = 6,
    F_CMP = 7,
    F_XOR = 8,
    F_ROR = 9,
    F_OR = 10,
    F_ROL = 11,
    F_SHR = 12,
    F_ASR = 13,
    F_AND = 14,
    F_BTST = 15
};

/* shared op body: computes result + flags; *store = 0 for cmp/btst */
static uint32_t alu_op(dsp3210_t *s, unsigned f, uint32_t a, uint32_t b, int w16, int *store) {
    uint32_t res = 0;
    unsigned msb = w16 ? 15 : 31;
    unsigned oldc = !!(s->ps & DSP3210_PS_c);
    *store = 1;

    switch (f) {
    case F_ADD:
        res = alu_add(s, a, b, 0, w16, 1);
        break;
    case F_SUB:
        res = alu_sub(s, a, b, w16, 1);
        break;
    case F_RSUB: /* N - rD / rS2 - rS1 */
        res = alu_sub(s, b, a, w16, 1);
        break;
    case F_CRADD: { /* carry propagates MSB → LSB */
        int nb = w16 ? 16 : 32;
        uint64_t sum = (uint64_t)bitrev(a & (w16 ? 0xFFFFu : ~0u), nb) + bitrev(b & (w16 ? 0xFFFFu : ~0u), nb);
        res = bitrev((uint32_t)sum & (w16 ? 0xFFFFu : ~0u), nb);
        if (w16)
            res = (uint32_t)sext16(res);
        set_flags(s, (res >> 31) & 1, (w16 ? (res & 0xFFFFu) : res) == 0, 0, (unsigned)(sum >> nb) & 1);
        break;
    }
    case F_ANDC:
        res = a & ~b;
        goto logic;
    case F_XOR:
        res = a ^ b;
        goto logic;
    case F_OR:
        res = a | b;
        goto logic;
    case F_AND:
        res = a & b;
        goto logic;
    case F_BTST:
        flags_nz(s, a & b, w16);
        *store = 0;
        break;
    case F_CMP:
        alu_sub(s, a, b, w16, 1);
        *store = 0;
        break;
    case F_SHL: /* logical; short zero-extends */
        res = (w16 ? (a << (b & 31)) & 0xFFFFu : a << (b & 31));
        flags_nz(s, res, w16);
        break;
    case F_SHR:
        res = (w16 ? (a & 0xFFFFu) : a) >> (b & 31);
        flags_nz(s, res, w16);
        break;
    case F_ASR: {
        int32_t sa = w16 ? (int32_t)sext16(a) : (int32_t)a;
        res = (uint32_t)(sa >> (b & 31));
        if (w16)
            res = (uint32_t)sext16(res);
        flags_nz(s, res, w16);
        break;
    }
    case F_ROL:
        /* rotate left through carry ≡ a+a+carry-in (the ADD page calls
         * rS*2 "a left shift by 1 with carry"); flags via add rules */
        res = alu_add(s, a, a, oldc, w16, 1);
        break;
    case F_ROR: { /* nzc, v = 0 */
        unsigned newc = a & 1u;
        uint32_t ua = w16 ? (a & 0xFFFFu) : a;
        res = (ua >> 1) | (oldc << msb);
        if (w16)
            res = (uint32_t)sext16(res);
        set_flags(s, (res >> 31) & 1, (w16 ? (res & 0xFFFFu) : res) == 0, 0, newc);
        break;
    }
    default: /* F_RES5: reserved function */
        *store = 0;
        break;
    }
    return res;

logic:
    if (w16)
        res = (uint32_t)sext16(res); /* AND-COMPLEMENT page: short
                                        results sign-extend */
    flags_nz(s, res, w16);
    return res;
}

static void exec_alu_reg(dsp3210_t *s, uint32_t w) {
    int w16 = !bits(w, 31, 31);
    unsigned f = bits(w, 24, 21);
    unsigned rd = bits(w, 20, 16);
    unsigned rs1 = bits(w, 15, 11);
    unsigned c = bits(w, 10, 5);
    unsigned rs2 = bits(w, 4, 0);
    uint32_t a, b, res;
    int store;

    if (!cond_eval(s, c))
        return; /* COND false: no store, no flags */

    a = opval(s, rs1);
    /* sp = sp++ / sp = sp-- move by 4, not 1 (INCR/DECR page) */
    if (f == F_ADD && rd == 25 && rs1 == 25 && (rs2 == 22 || rs2 == 23))
        b = rs2 == 23 ? 4u : (uint32_t)-4;
    else
        b = opval(s, rs2); /* +n/-n read as ±1 */

    res = alu_op(s, f, a, b, w16, &store);
    if (store)
        regw(s, rd, res);
}

static void exec_alu_imm(dsp3210_t *s, uint32_t w) {
    int w16 = !bits(w, 31, 31);
    unsigned f = bits(w, 24, 21);
    unsigned rd = bits(w, 20, 16);
    uint32_t n = (uint32_t)sext16(w);
    uint32_t a = opval(s, rd), res;
    int store;

    /* rotates have no immediate operand — they rotate rD by 1 */
    if (f == F_ROL || f == F_ROR)
        n = a;
    res = alu_op(s, f, a, n, w16, &store);
    if (store)
        regw(s, rd, res);
}

/* ------------------------------------------------------------------ */
/* CA moves (formats 7a-7d)                                            */

static int exec_move(dsp3210_t *s, uint32_t w) {
    int direct = !bits(w, 31, 31); /* 7a */
    unsigned io = bits(w, 25, 25); /* 7d */
    unsigned t = bits(w, 24, 24); /* 0 = load register, 1 = store */
    unsigned wsz = bits(w, 23, 21);
    unsigned rh = bits(w, 20, 16);
    int size = w_size(wsz);
    uint32_t raw;

    /* NOTE: CA register loads SET the CAU flags from the loaded value.
     * The LOAD page's header claims "CAU FLAGS AFFECTED: None", but its
     * own restriction reads "the CAU register loaded and THE FLAGS SET
     * AS A RESULT OF THE LOAD cannot be referenced in the following
     * instruction" [IM §4.4.1.3 Restriction 3] — and Apple's AppleSRC
     * module branches on `rD = *rD ; nop ; if (gt)`, which only works
     * if loads set flags.  n/z from the extended value; v = c = 0
     * (exact v/c behaviour undocumented). */

    if (direct) {
        /* 7a: rH = (w) *L / *L = (w) rH.  L is a 16-bit on-chip window
         * address: upper half $0000 (computer) or $5003 (processor)
         * [IM §3.5.6]. */
        uint32_t addr = (s->pcw & (1u << 10)) ? (w & 0xFFFFu) : 0x50030000u | (w & 0xFFFFu);
        if (bits(w, 25, 25))
            return DSP3210_STEP_OK; /* reserved encoding */
        if (t == 0) {
            if (mem_read(s, addr, size, &raw))
                return DSP3210_STEP_OK;
            regw(s, rh, w_extend(wsz, raw));
            flags_nz(s, w_extend(wsz, raw), 0);
        } else {
            mem_write(s, addr, size, w_select(wsz, opval(s, rh)));
        }
        return DSP3210_STEP_OK;
    }

    if (!io && bits(w, 10, 10)) {
        /* 7b: rH = (w) iorN / iorN = (w) rH — and the three spc
         * pseudo-instructions (waiti/bkpt/sftrst), which differ only in
         * the W field [DOC §1.5.4] */
        unsigned ior = w & 31;
        if (t == 0) {
            regw(s, rh, w_extend(wsz, ior_read(s, ior)));
            flags_nz(s, w_extend(wsz, ior_read(s, ior)), 0);
        } else {
            int spc = ior_write(s, ior, w_select(wsz, opval(s, rh)), size);
            if (spc == SPC_SFTRST) { /* drop to base level [SFTRST] */
                s->level = DSP3210_LVL_BASE;
                s->error_nonmaskable = 0;
            } else if (spc == SPC_BKPT) {
                return DSP3210_STEP_BKPT;
            } else if (spc == SPC_WAITI) {
                /* ignored if an interrupt is already pending [WAITI] */
                if (!pending_vector(s))
                    s->waiting = 1;
            }
        }
        return DSP3210_STEP_OK;
    }

    /* 7c: rH <-> MEM  |  7d: iorH <-> MEM */
    {
        unsigned rp = bits(w, 15, 11);
        unsigned ri = w & 31;
        uint32_t addr = opval(s, rp);

        if (t == 0) {
            if (mem_read(s, addr, size, &raw))
                return DSP3210_STEP_OK;
            ca_postmod(s, rp, ri, size);
            if (io) {
                int spc = ior_write(s, rh, raw, size); /* iorD = MEM:
                      (byte)/(short) affect only the low bits (LOAD-IOR),
                      approximated here as a plain sized write */
                (void)spc; /* spc as 7d destination: undefined;
                              side effect not honoured */
            } else {
                regw(s, rh, w_extend(wsz, raw));
                flags_nz(s, w_extend(wsz, raw), 0);
            }
        } else {
            uint32_t v = io ? w_select(wsz, ior_read(s, rh)) : w_select(wsz, opval(s, rh));
            if (mem_write(s, addr, size, v))
                return DSP3210_STEP_OK;
            ca_postmod(s, rp, ri, size);
        }
    }
    return DSP3210_STEP_OK;
}

/* ------------------------------------------------------------------ */
/* main execute                                                        */

static int opcode_is_illegal(unsigned op6) {
    switch (op6) { /* IM §7.5.3.2, the seven patterns */
    case 0x00:
    case 0x01:
    case 0x02:
    case 0x0F:
    case 0x16:
    case 0x17:
    case 0x22:
        return 1;
    default:
        return 0;
    }
}

static int exec_insn(dsp3210_t *s, uint32_t w) {
    unsigned op6 = w >> 26;
    uint32_t addr = s->cur_insn;

    if (opcode_is_illegal(op6)) {
        take_error(s, DSP3210_VEC_ILLOP);
        return DSP3210_STEP_OK;
    }
    if ((w >> 29) >= 1 && (w >> 29) <= 3)
        return exec_da(s, w);

    switch (op6) {
    case 0x03: { /* 3a: if (rM-- >= 0) goto — test
                    the old value, always decrement
                    [GOTO-LOOP] */
        unsigned rm = bits(w, 25, 21), rb = bits(w, 20, 16);
        uint32_t val = opval(s, rm);
        if ((int32_t)val >= 0)
            s->npc = opval(s, rb) + (uint32_t)sext16(w);
        regw(s, rm, val - 1);
        break;
    }
    case 0x04: { /* 4a: call {rB,rB+N} (rM); rM gets
                    the address after the latent
                    instruction = insn + 8 [CALL] */
        unsigned rm = bits(w, 25, 21), rb = bits(w, 20, 16);
        uint32_t target = opval(s, rb) + (uint32_t)sext16(w);
        regw(s, rm, addr + 8);
        s->npc = target;
        break;
    }
    case 0x05:
    case 0x25: { /* 5a/5b: rD = (size) rS3 + N,
                    flags nzvc [ADD] */
        unsigned rd = bits(w, 25, 21), rs3 = bits(w, 20, 16);
        int w16 = !(op6 & 0x20);
        regw(s, rd, alu_add(s, opval(s, rs3), (uint32_t)sext16(w), 0, w16, 1));
        break;
    }
    case 0x06:
    case 0x26: /* 6a-6d: ALU */
        if (bits(w, 25, 25))
            exec_alu_imm(s, w);
        else
            exec_alu_reg(s, w);
        break;
    case 0x07:
    case 0x27: /* 7a-7d: moves */
        return exec_move(s, w);
    case 0x20:
    case 0x21: { /* 0b/1b: if (COND) goto / nop /
                    ireturn */
        unsigned c = bits(w, 26, 21), rb = bits(w, 20, 16);
        if (c == 1 && rb == 30 && (w & 0xFFFFu) == 0) {
            do_ireturn(s);
            break;
        }
        if (cond_eval(s, c))
            s->npc = opval(s, rb) + (uint32_t)sext16(w);
        break;
    }
    case 0x23: { /* 3b/3c: do / dolock / doblock */
        unsigned isreg = bits(w, 25, 25);
        unsigned b = bits(w, 24, 24), m = bits(w, 23, 23);
        unsigned k = bits(w, 17, 11);
        uint32_t count = isreg ? (opval(s, w & 31) & 0x7FFu) + 1 : bits(w, 10, 0) + 1;
        if (b && m)
            break; /* reserved combination */
        if (m)
            k = 0; /* doblock: single instruction */
        s->do_active = 1;
        s->do_lock = (b != 0); /* dolock masks interrupts */
        s->do_start = s->pc; /* next instruction */
        s->do_end = s->pc + 4u * k; /* K+1 instructions */
        s->do_count = count; /* L+1 / (rM&0x7FF)+1 iterations */
        break;
    }
    case 0x24: { /* 4b: rD = rS <<| N — N<<16 | rS,
                    always long, flags nz [SHIFT-OR] */
        unsigned rd = bits(w, 25, 21), rs = bits(w, 20, 16);
        uint32_t res = opval(s, rs) | ((w & 0xFFFFu) << 16);
        flags_nz(s, res, 0);
        regw(s, rd, res);
        break;
    }
    case 0x28:
    case 0x29:
    case 0x2A:
    case 0x2B:
    case 0x2C:
    case 0x2D:
    case 0x2E:
    case 0x2F: { /* 8a: goto {M, rB+M} */
        unsigned rb = bits(w, 20, 16);
        uint32_t mm = (bits(w, 28, 21) << 16) | (w & 0xFFFFu);
        s->npc = opval(s, rb) + mm;
        break;
    }
    case 0x30:
    case 0x31:
    case 0x32:
    case 0x33:
    case 0x34:
    case 0x35:
    case 0x36:
    case 0x37: { /* 8b: rD=(ushort24)M,
        no flags [SET24] */
        unsigned rd = bits(w, 20, 16);
        regw(s, rd, (bits(w, 28, 21) << 16) | (w & 0xFFFFu));
        break;
    }
    case 0x38:
    case 0x39:
    case 0x3A:
    case 0x3B:
    case 0x3C:
    case 0x3D:
    case 0x3E:
    case 0x3F: { /* 8c: call M (rM) */
        unsigned rm = bits(w, 20, 16);
        regw(s, rm, addr + 8);
        s->npc = (bits(w, 28, 21) << 16) | (w & 0xFFFFu);
        break;
    }
    default: /* unreachable */
        break;
    }
    return DSP3210_STEP_OK;
}

/* ------------------------------------------------------------------ */
/* step                                                                */

int dsp3210_step(dsp3210_t *s) {
    uint32_t w, addr;
    int st;

    if (s->level == DSP3210_LVL_DERROR)
        return DSP3210_STEP_DERROR;
    if (s->halted)
        return DSP3210_STEP_BKPT; /* dead until reset */

    if (s->waiting) {
        if (pending_vector(s)) {
            /* the instruction after waiti executes when the interrupt
             * is recognised, before the interrupt is taken [WAITI] */
            s->waiting = 0;
            s->int_defer = 1;
        } else {
            return DSP3210_STEP_WAITI;
        }
    }

    /* interrupt recognition: base level only, never in the shadow of a
     * delayed branch (branches are not interruptible [IM §7.5.1]), never
     * inside a dolock loop, and not before a pending irsh replay */
    if (!s->int_defer && !s->irsh_pending && s->level == DSP3210_LVL_BASE && s->npc == s->pc + 4 &&
        !(s->do_active && s->do_lock)) {
        int vn = pending_vector(s);
        if (vn)
            take_interrupt(s, vn);
    }
    s->int_defer = 0;

    if (s->irsh_pending) {
        /* replay the shadowed instruction; pc/npc already aim at pcsh */
        s->irsh_pending = 0;
        addr = s->irsh_addr;
        w = s->irsh;
        s->cur_insn = addr;
        s->prefetch_addr = 1; /* odd: never matches a pc */
        s->icount++;
        st = exec_insn(s, w);
    } else {
        addr = s->pc;
        s->cur_insn = addr;
        s->in_fetch = 1;
        if (mem_read(s, addr, 4, &w)) {
            s->in_fetch = 0;
            return DSP3210_STEP_OK;
        }
        s->in_fetch = 0;

        s->pc = s->npc;
        s->npc = s->pc + 4;
        s->icount++;

        /* model the prefetch of the next instruction: it happens under
         * the memory map in force NOW, before this instruction's side
         * effects (a pcw write) can change it — this is what lands in
         * irsh if an interrupt hits at the next boundary */
        s->prefetch_addr = s->pc;
        if (safe_read(s, s->pc, 4, &s->prefetch))
            s->prefetch_addr = 1;

        st = exec_insn(s, w);
    }

    /* do-loop back-edge [DO page]: after the last instruction of the
     * body, loop while iterations remain.  (An error exception inside
     * the body clears do_active, so this is skipped on abort.) */
    if (s->do_active && addr == s->do_end) {
        if (--s->do_count == 0) {
            s->do_active = 0;
        } else {
            s->pc = s->do_start;
            s->npc = s->do_start + 4;
        }
    }
    /* the DAU multiplier-input pipeline advances one instruction cycle
     * [IM §4.4.2.2] — including on instructions that touch no accumulator */
    memmove(s->a_pipe[1], s->a_pipe[0], sizeof s->a_pipe[0] * 2);
    memcpy(s->a_pipe[0], s->a, sizeof s->a);
    /* ...and so does the DAU flag pipeline [IM §4.4.2.4] */
    s->dau_flag_pipe[3] = s->dau_flag_pipe[2];
    s->dau_flag_pipe[2] = s->dau_flag_pipe[1];
    s->dau_flag_pipe[1] = s->dau_flag_pipe[0];
    s->dau_flag_pipe[0] = (uint16_t)(s->ps & DSP3210_PS_DAU);

    /* the on-chip timer counts per executed instruction cycle */
    timer_tick(s, timer_ticks_per_insn(s));
    ext_pulse_tick(s); /* live pin-pulse windows burn down in core time */
    if (st == DSP3210_STEP_BKPT)
        s->halted = 1; /* crashed until reset */
    return st;
}

/* ------------------------------------------------------------------ */
/* burn-down execution (the aux-core sprint ABI, cores.md)             */

/* True when the timer is counting from an internal clock — the only wake
 * source a sleeping kernel has besides board interrupts. */
static int timer_running(const dsp3210_t *s) {
    return timer_ticks_per_insn(s) > 0;
}

void dsp3210_run(dsp3210_t *s, uint32_t *instructions) {
    while (*instructions > 0) {
        int st;
        if (s->halted || s->level == DSP3210_LVL_DERROR)
            return; /* crashed: budget unspent */
        st = dsp3210_step(s);
        if (st == DSP3210_STEP_WAITI) {
            if (!timer_running(s))
                return; /* parked: budget unspent */
            /* asleep with the timer counting: time passes, no instruction
             * retires — charge the budget while the timer runs to wake */
            timer_tick(s, timer_ticks_per_insn(s));
            ext_pulse_tick(s);
            (*instructions)--;
            continue;
        }
        (*instructions)--;
        if (st == DSP3210_STEP_BKPT || st == DSP3210_STEP_DERROR)
            return;
    }
}

int dsp3210_is_idle(const dsp3210_t *s) {
    if (s->halted || s->level == DSP3210_LVL_DERROR)
        return 1;
    if (s->waiting && !(s->pending & s->emr & 0xFF00u) && !timer_running(s))
        return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* host interface                                                      */

void dsp3210_reset(dsp3210_t *s, unsigned straps) {
    /* Reset state per [IM Table 7-4]; r1-r19, r21, a0-a3, ps, ctr are
     * architecturally undefined — cleared here for determinism. */
    uint32_t prev_pc = s->pc;
    memset(s->r, 0, sizeof s->r);
    memset(s->a, 0, sizeof s->a); /* e = 0 => all four read as zero */
    memset(s->a_pipe, 0, sizeof s->a_pipe);
    memset(s->dau_flag_pipe, 0, sizeof s->dau_flag_pipe);
    memset(s->sh_dau_flag_pipe, 0, sizeof s->sh_dau_flag_pipe);
    memset(s->da_wr, 0, sizeof s->da_wr);
    s->da_wr_n = 0;
    s->in_fetch = 0;
    memset(s->sh_a_pipe, 0, sizeof s->sh_a_pipe);
    s->r[20] = prev_pc; /* r20 = previous pc */
    s->pc = 0; /* reset vector: call evtp+0 (r20),
                  evtp cleared to 0 */
    s->npc = 4;
    s->pcsh = 0;
    s->ps = 0;
    s->emr = 0;
    s->pcw = (uint16_t)(0x38F | ((straps & 0xFu) << 10));
    s->pcw_locked = 0;
    s->dauc = 0;
    s->ctr = 0;
    s->do_active = 0;
    s->do_lock = 0;
    s->level = DSP3210_LVL_BASE;
    s->error_nonmaskable = 0;
    s->pending = 0;
    s->waiting = 0;
    s->int_defer = 0;
    s->irsh_pending = 0;
    s->irsh = 0;
    s->irsh_addr = 0;
    s->prefetch_addr = 1;
    s->last_vector = -1;
    s->cur_insn = 0;
    s->halted = 0;
    /* on-chip peripherals [IM Table 7-4]: timer held at $FFFFFFFF, all
     * BIO pins inputs with the output register cleared */
    s->tcon = 0;
    s->timer_count = 0xFFFFFFFFu;
    s->timer_reload = 0xFFFFFFFFu;
    s->bioc = 0;
    s->bio_out = 0;
    /* EXT pins idle negated — PS.IR0/IR1 read 1 */
    s->ext_pulse[0] = 0;
    s->ext_pulse[1] = 0;
    update_irq_ps(s);
}

void dsp3210_init(dsp3210_t *s, uint8_t *mem, uint32_t mem_size) {
    memset(s, 0, sizeof *s);
    s->mem = mem;
    s->mem_size = mem_size;
    dsp3210_reset(s, 0);
}

void dsp3210_request_interrupt(dsp3210_t *s, int vector) {
    if (vector >= 8 && vector <= 15)
        s->pending |= (uint16_t)(1u << vector);
}

void dsp3210_ext_pulse(dsp3210_t *s, int vector, uint32_t slots) {
    int idx;
    if (vector == DSP3210_VEC_EXT0)
        idx = 0;
    else if (vector == DSP3210_VEC_EXT1)
        idx = 1;
    else
        return;
    s->pending |= (uint16_t)(1u << vector); /* edge-latch the request */
    s->ext_pulse[idx] = slots ? slots : 1;
    update_irq_ps(s); /* pin asserted for the window */
}

int dsp3210_load(dsp3210_t *s, uint32_t addr, const void *buf, size_t len) {
    const uint8_t *b = (const uint8_t *)buf;
    size_t i;
    for (i = 0; i < len; i++) {
        uint8_t *p = mem_ptr(s, addr + (uint32_t)i);
        if (!p)
            return -1;
        *p = b[i];
    }
    return 0;
}

int dsp3210_peek(dsp3210_t *s, uint32_t addr, int size, uint32_t *out) {
    return raw_access(s, addr, size, out, 0);
}

int dsp3210_poke(dsp3210_t *s, uint32_t addr, int size, uint32_t val) {
    return raw_access(s, addr, size, &val, 1);
}

double dsp3210_acc_get(const dsp3210_t *s, int n) {
    return dsp3210_acc_double(s->a[n & 3]);
}

void dsp3210_acc_set(dsp3210_t *s, int n, double v) {
    int k;
    s->a[n & 3] = dsp3210_acc_from_double(v);
    /* a host poke is instantaneous: flush the multiplier pipeline too, so
     * rigs and tests see the value on the very next instruction */
    for (k = 0; k < 3; k++)
        s->a_pipe[k][n & 3] = s->a[n & 3];
}

void dsp3210_acc_raw(const dsp3210_t *s, int n, int64_t *mant_guard, int *exp) {
    dsp3210_acc a = s->a[n & 3];
    if (mant_guard) {
        /* the stored bits 39-8: the implicit bit is not stored, so the
         * field is the value minus (+/-)1 at the mantissa's scale */
        int64_t m = a.m;
        *mant_guard = (a.e == 0) ? 0 : (m - (m < 0 ? -(INT64_C(1) << SC_ACC) : (INT64_C(1) << SC_ACC))) & 0xFFFFFFFFu;
    }
    if (exp)
        *exp = a.e;
}

const char *dsp3210_step_name(int status) {
    switch (status) {
    case DSP3210_STEP_OK:
        return "ok";
    case DSP3210_STEP_WAITI:
        return "waiti";
    case DSP3210_STEP_BKPT:
        return "bkpt";
    case DSP3210_STEP_DERROR:
        return "double-error";
    default:
        return "?";
    }
}
