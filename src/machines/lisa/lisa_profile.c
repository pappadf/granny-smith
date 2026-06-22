// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// lisa_profile.c
// Apple ProFile parallel hard disk.  See lisa_profile.h and docs/machines/lisa/lisa.md §14.
//
// Behavioural model of the ProFile controller's byte-at-a-time handshake,
// reverse-engineered from Apple's own drivers (boot ROM RM248.B and OS
// SOURCE-PROFILEASM).  The host and controller exchange a state byte over the
// no-handshake port-A register and clock each data byte over the handshaked
// register; the BSY line (PB1/CA1) and CMD/ line (PB4) sequence the phases:
//
//   IDLE --CMD/ asserted-->  handshake(state $01) --reply $55-->  receive 6
//   command bytes --CMD/ asserted--> handshake(state $02 read / $03 write)
//   --reply $55-->  READ: stream 4 status + 532 block bytes
//                   WRITE: receive 532 block bytes, handshake(state $06),
//                          then stream 4 status bytes
//
// The controller drives BSY low when it sees CMD/ asserted and raises it once
// the host has read the state byte, sent its reply, and deasserted CMD/.  We do
// this synchronously: the boot ROM polls the BSY level, the OS the CA1 edge.

#include "lisa_profile.h"

#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("profile")

// On-the-wire block geometry (docs/machines/lisa/lisa.md §14): 20-byte tag/header + 512 data.
#define PRO_TAG    20
#define PRO_DATA   512
#define PRO_BLOCK  (PRO_TAG + PRO_DATA) // 532
#define PRO_STATUS 4 // status bytes preceding a read block
#define PRO_RDLEN  (PRO_STATUS + PRO_BLOCK)

// Standard 5 MB ProFile: 9728 logical blocks (the canonical device the Lisa
// Office System and boot ROM were written for).  A pre-existing image's size
// overrides this.
#define PRO_DEFAULT_BLOCKS 9728u

// The controller's reserved device-info / spare-table block.
#define PRO_INFO_BLOCK 0xFFFFFFu

// Port-B control lines (docs/machines/lisa/lisa.md §14): CMD/ = PB4 (0 = asserted), DRW = PB3.
#define PB_CMD 0x10
#define PB_DRW 0x08

// Command opcodes (command-frame byte 0).
#define PRO_OP_READ         0x00
#define PRO_OP_WRITE        0x01
#define PRO_OP_WRITE_VERIFY 0x02

// State bytes the controller presents during each handshake.
#define ST_READY  0x01 // ready to accept a command
#define ST_READ   0x02 // command accepted, ready to send data
#define ST_WRITE  0x03 // command accepted, ready to receive data
#define ST_WRDONE 0x06 // data received, write complete

typedef enum {
    PH_IDLE, // ready; next CMD/ assert → present $01
    PH_HS, // CMD/ asserted; presenting state byte, awaiting reply+deassert
    PH_RECV_CMD, // collecting the 6-byte command frame (strobed writes)
    PH_CMD_DONE, // command parsed; awaiting the 2nd handshake
    PH_READ, // streaming status + block bytes (strobed reads)
    PH_WRITE, // receiving block bytes (strobed writes)
    PH_WRITE_DONE, // block received; awaiting the completion handshake
    PH_RD_STATUS, // streaming the 4 post-write status bytes (strobed reads)
} pro_phase_t;

struct lisa_profile {
    lisa_profile_bsy_fn bsy_cb;
    void *bsy_ctx;

    uint8_t *data; // nblocks * PRO_BLOCK, or NULL when detached
    uint32_t nblocks;
    char *path;
    bool writable;
    bool dirty;

    // Handshake / transfer state.
    pro_phase_t phase;
    bool cmd_asserted; // last CMD/ level (true = asserted/low)
    uint8_t state_byte; // value presented on the no-handshake port during PH_HS
    uint8_t after_hs; // pro_phase_t to enter once the handshake completes OK
    uint8_t reply; // last reply byte the host wrote (no-handshake register)

    uint8_t cmd[6]; // command frame being collected
    int cmd_idx;

    uint8_t buf[PRO_RDLEN]; // current transfer buffer (status+block / block / status)
    int buf_len;
    int buf_idx;
};

// === BSY line ===============================================================

static void pro_set_busy(lisa_profile_t *pf, bool busy) {
    if (pf->bsy_cb)
        pf->bsy_cb(pf->bsy_ctx, busy);
}

// === Block I/O against the in-RAM image =====================================

// Fill `pf->buf` with [4 status][532 block] for a read of `block`.  Block
// $FFFFFF returns the synthesized device-info block (capacity + drive type),
// exactly the way the real controller firmware does (it is not platter data).
static void pro_fill_read(lisa_profile_t *pf, uint32_t block) {
    memset(pf->buf, 0, PRO_RDLEN); // status bytes 0 = OK; block zeroed by default

    if (block == PRO_INFO_BLOCK) {
        // The controller synthesizes this block.  The OS PROF_INIT reads it as a
        // byte stream right after the 4 status bytes (it does not split off a
        // 20-byte header), taking the drive type at offset 14 and the block
        // count (24-bit BE) at offsets 18..20.  Conventional layout: 13-byte
        // device name, 3-byte device number (0 → drive type 0 = ProFile), 2-byte
        // firmware rev, 3-byte block count, 2-byte bytes/block, spare count.
        uint8_t *info = &pf->buf[PRO_STATUS];
        memcpy(info, "PROFILE      ", 13); // 0..12: device name
        // 13..15: device number = 0 → byte 14 (drive type) = 0 = ProFile
        info[16] = 0x03; // 16..17: firmware revision (cosmetic)
        info[17] = 0x98;
        info[18] = (uint8_t)(pf->nblocks >> 16); // 18..20: block count (BE)
        info[19] = (uint8_t)(pf->nblocks >> 8);
        info[20] = (uint8_t)(pf->nblocks);
        info[21] = (uint8_t)(PRO_BLOCK >> 8); // 21..22: bytes per block
        info[22] = (uint8_t)(PRO_BLOCK);
        info[23] = 32; // 23: spare-block count (cosmetic)
        LOG(2, "device-info: %u blocks", pf->nblocks);
    } else if (pf->data && block < pf->nblocks) {
        memcpy(&pf->buf[PRO_STATUS], &pf->data[(size_t)block * PRO_BLOCK], PRO_BLOCK);
    } else {
        pf->buf[1] = 0x02; // out-of-range → flag in status (non-fatal here)
        LOG(1, "read block %u out of range (%u blocks)", block, pf->nblocks);
    }
    pf->buf_len = PRO_RDLEN;
    pf->buf_idx = 0;
}

// Commit the 532-byte write buffer to `block`.
static void pro_commit_write(lisa_profile_t *pf, uint32_t block) {
    if (block == PRO_INFO_BLOCK) {
        LOG(1, "ignoring write to device-info block");
        return;
    }
    if (!pf->data || block >= pf->nblocks) {
        LOG(1, "write block %u out of range (%u blocks)", block, pf->nblocks);
        return;
    }
    memcpy(&pf->data[(size_t)block * PRO_BLOCK], pf->buf, PRO_BLOCK);
    pf->dirty = true;
    LOG(2, "wrote block %u", block);
}

// === Command frame ==========================================================

static uint32_t pro_cmd_block(const lisa_profile_t *pf) {
    return ((uint32_t)pf->cmd[1] << 16) | ((uint32_t)pf->cmd[2] << 8) | pf->cmd[3];
}

// All 6 command bytes are in; decide the next handshake's state byte + phase.
static void pro_parse_command(lisa_profile_t *pf) {
    uint8_t op = pf->cmd[0];
    uint32_t block = pro_cmd_block(pf);
    if (op == PRO_OP_READ) {
        pro_fill_read(pf, block);
        pf->state_byte = ST_READ;
        pf->after_hs = PH_READ;
        LOG(2, "cmd READ block %u", block);
    } else { // write / write-verify
        pf->buf_len = PRO_BLOCK;
        pf->buf_idx = 0;
        pf->state_byte = ST_WRITE;
        pf->after_hs = PH_WRITE;
        LOG(2, "cmd WRITE block %u", block);
    }
    pf->phase = PH_CMD_DONE;
}

// === Control-line edges =====================================================

// Enter the phase that follows a successful ($55) handshake.
static void pro_enter(lisa_profile_t *pf, pro_phase_t phase) {
    pf->phase = phase;
    switch (phase) {
    case PH_RECV_CMD:
        pf->cmd_idx = 0;
        break;
    case PH_READ:
        pf->buf_idx = 0; // buffer already filled by pro_parse_command
        break;
    case PH_WRITE:
        pf->buf_idx = 0;
        pf->buf_len = PRO_BLOCK;
        break;
    case PH_RD_STATUS:
        memset(pf->buf, 0, PRO_STATUS); // 4 status bytes, all OK
        pf->buf_idx = 0;
        pf->buf_len = PRO_STATUS;
        break;
    default:
        break;
    }
}

void lisa_profile_portb(lisa_profile_t *pf, uint8_t portb) {
    if (!pf || !pf->data)
        return;
    bool asserted = (portb & PB_CMD) == 0; // CMD/ active low

    if (asserted == pf->cmd_asserted)
        return; // no edge
    pf->cmd_asserted = asserted;

    if (asserted) {
        // CMD/ falling edge: controller goes busy and presents its state byte.
        if (pf->phase != PH_CMD_DONE && pf->phase != PH_WRITE_DONE) {
            // Start of a fresh transaction (from IDLE or after a finished one).
            pf->state_byte = ST_READY;
            pf->after_hs = PH_RECV_CMD;
        }
        pf->phase = PH_HS;
        pro_set_busy(pf, true);
        LOG(3, "CMD/ asserted, present state $%02x", pf->state_byte);
    } else {
        // CMD/ rising edge: the host has read the state byte and sent its reply.
        pro_set_busy(pf, false);
        LOG(3, "CMD/ deasserted, reply $%02x", pf->reply);
        if (pf->phase == PH_HS) {
            if (pf->reply == 0x55)
                pro_enter(pf, (pro_phase_t)pf->after_hs); // proceed
            else
                pf->phase = PH_IDLE; // host declined / probe handshake
        }
    }
}

// === Port-A data ============================================================

uint8_t lisa_profile_porta_read(lisa_profile_t *pf, bool handshake) {
    if (!pf || !pf->data)
        return 0xFF;

    if (!handshake) {
        // No-handshake register: the host samples the current state byte.
        uint8_t v = (pf->phase == PH_HS) ? pf->state_byte : 0xFF;
        return v;
    }

    // Handshaked register: clock out the next data byte.
    if ((pf->phase == PH_READ || pf->phase == PH_RD_STATUS) && pf->buf_idx < pf->buf_len) {
        uint8_t b = pf->buf[pf->buf_idx++];
        if (pf->phase == PH_RD_STATUS && pf->buf_idx >= pf->buf_len)
            pf->phase = PH_IDLE; // status drained → transaction done
        return b;
    }
    return 0xFF;
}

void lisa_profile_porta_write(lisa_profile_t *pf, uint8_t value, bool handshake) {
    if (!pf || !pf->data)
        return;

    if (!handshake) {
        // No-handshake register: the host's handshake reply ($55 = proceed).
        pf->reply = value;
        return;
    }

    // Handshaked register: a command byte or a write-data byte.
    if (pf->phase == PH_RECV_CMD) {
        if (pf->cmd_idx < (int)sizeof(pf->cmd)) {
            pf->cmd[pf->cmd_idx++] = value;
            if (pf->cmd_idx == (int)sizeof(pf->cmd))
                pro_parse_command(pf);
        }
    } else if (pf->phase == PH_WRITE) {
        if (pf->buf_idx < pf->buf_len) {
            pf->buf[pf->buf_idx++] = value;
            if (pf->buf_idx >= pf->buf_len) {
                pro_commit_write(pf, pro_cmd_block(pf));
                pf->state_byte = ST_WRDONE;
                pf->after_hs = PH_RD_STATUS;
                pf->phase = PH_WRITE_DONE;
            }
        }
    }
}

void lisa_profile_reset(lisa_profile_t *pf) {
    if (!pf)
        return;
    pf->phase = PH_IDLE;
    pf->cmd_asserted = false;
    pf->cmd_idx = 0;
    pf->buf_idx = 0;
    pro_set_busy(pf, false); // ready after reset
    LOG(2, "controller reset");
}

// === Media ==================================================================

bool lisa_profile_attached(const lisa_profile_t *pf) {
    return pf && pf->data != NULL;
}
bool lisa_profile_connected(const lisa_profile_t *pf) {
    return lisa_profile_attached(pf);
}

bool lisa_profile_flush(lisa_profile_t *pf) {
    if (!pf || !pf->data || !pf->path || !pf->writable || !pf->dirty)
        return true;
    FILE *f = fopen(pf->path, "r+b");
    if (!f)
        f = fopen(pf->path, "wb");
    if (!f) {
        LOG(1, "flush: cannot open %s", pf->path);
        return false;
    }
    size_t bytes = (size_t)pf->nblocks * PRO_BLOCK;
    size_t put = fwrite(pf->data, 1, bytes, f);
    fclose(f);
    pf->dirty = (put != bytes);
    return put == bytes;
}

bool lisa_profile_save_as(const lisa_profile_t *pf, const char *path) {
    if (!pf || !pf->data || !path || !*path)
        return false;
    FILE *f = fopen(path, "wb");
    if (!f) {
        LOG(1, "save_as: cannot open %s", path);
        return false;
    }
    size_t bytes = (size_t)pf->nblocks * PRO_BLOCK;
    size_t put = fwrite(pf->data, 1, bytes, f);
    fclose(f);
    if (put != bytes) {
        LOG(1, "save_as: short write to %s (%zu/%zu)", path, put, bytes);
        remove(path); // don't leave a truncated image behind
        return false;
    }
    LOG(2, "save_as: wrote %u-block image to %s", pf->nblocks, path);
    return true;
}

bool lisa_profile_attach(lisa_profile_t *pf, const char *path, bool writable) {
    if (!pf)
        return false;
    lisa_profile_detach(pf);

    uint32_t nblocks = PRO_DEFAULT_BLOCKS;
    uint8_t *data = NULL;

    FILE *f = path ? fopen(path, "rb") : NULL;
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0 && (sz % PRO_BLOCK) == 0)
            nblocks = (uint32_t)(sz / PRO_BLOCK);
        data = (uint8_t *)calloc((size_t)nblocks * PRO_BLOCK, 1);
        if (data && sz > 0) {
            size_t want = (size_t)nblocks * PRO_BLOCK;
            if (fread(data, 1, want, f) != want)
                LOG(1, "attach: short read of %s", path);
        }
        fclose(f);
    } else {
        // No existing file → blank disk of the default geometry.
        data = (uint8_t *)calloc((size_t)nblocks * PRO_BLOCK, 1);
    }
    if (!data) {
        LOG(1, "attach: out of memory");
        return false;
    }

    pf->data = data;
    pf->nblocks = nblocks;
    pf->writable = writable;
    pf->dirty = false;
    pf->path = path ? strdup(path) : NULL;
    lisa_profile_reset(pf);
    LOG(2, "attach %s (%u blocks, %s)", path ? path : "(blank)", nblocks, writable ? "rw" : "ro");
    return true;
}

void lisa_profile_detach(lisa_profile_t *pf) {
    if (!pf || !pf->data)
        return;
    lisa_profile_flush(pf);
    free(pf->data);
    pf->data = NULL;
    free(pf->path);
    pf->path = NULL;
    pf->nblocks = 0;
    pf->dirty = false;
    pf->phase = PH_IDLE;
}

// === Lifecycle =============================================================

lisa_profile_t *lisa_profile_init(lisa_profile_bsy_fn bsy_cb, void *bsy_ctx, checkpoint_t *cp) {
    lisa_profile_t *pf = (lisa_profile_t *)calloc(1, sizeof(*pf));
    if (!pf)
        return NULL;
    pf->bsy_cb = bsy_cb;
    pf->bsy_ctx = bsy_ctx;
    pf->phase = PH_IDLE;
    if (cp)
        lisa_profile_checkpoint(pf, cp);
    return pf;
}

void lisa_profile_delete(lisa_profile_t *pf) {
    if (!pf)
        return;
    lisa_profile_detach(pf);
    free(pf);
}

void lisa_profile_checkpoint(lisa_profile_t *pf, checkpoint_t *cp) {
    // Symmetric no-op (same discipline as lisa_fdc/MMU): the handshake state is
    // re-derived by the next transaction; image bytes live in the backing file.
    (void)pf;
    (void)cp;
}
