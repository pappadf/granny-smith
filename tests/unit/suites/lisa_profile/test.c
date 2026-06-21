// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// Unit test for the Lisa ProFile parallel hard disk (lisa_profile.c).
//
// Drives the device exactly as the VIA would during a transaction (CMD/ edges
// on port B via lisa_profile_portb; state/reply bytes on the no-handshake
// register; data/command/status bytes on the handshaked register) and checks
// the full handshake sequence, the synthesized device-info block, and a
// write→read round-trip.  Deterministic, no emulator/ROM/MMU required.

#include "lisa_profile.h"
#include "test_assert.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Port-B control levels (PB4 = CMD/, active low; PB3 = DRW).
#define CMD_ASSERT   0x08 // PB4 = 0 (asserted), PB3 = 1
#define CMD_DEASSERT 0x18 // PB4 = 1 (deasserted), PB3 = 1

#define PRO_BLOCK 532
#define PRO_RDLEN (4 + PRO_BLOCK) // 4 status bytes + block

static bool g_busy; // last BSY state reported by the device
static void bsy_cb(void *ctx, bool busy) {
    (void)ctx;
    g_busy = busy;
}

// One handshake: assert CMD/, sample the state byte, send `reply`, deassert.
// Returns the state byte the controller presented.
static uint8_t handshake(lisa_profile_t *pf, uint8_t reply) {
    lisa_profile_portb(pf, CMD_ASSERT);
    ASSERT_TRUE(g_busy); // controller drives BSY low on CMD/ assert
    uint8_t state = lisa_profile_porta_read(pf, false); // no-handshake register
    lisa_profile_porta_write(pf, reply, false); // reply byte
    lisa_profile_portb(pf, CMD_DEASSERT);
    ASSERT_TRUE(!g_busy); // BSY released on deassert
    return state;
}

static void send_command(lisa_profile_t *pf, uint8_t op, uint32_t block) {
    uint8_t cmd[6] = {op, (uint8_t)(block >> 16), (uint8_t)(block >> 8), (uint8_t)block, 0x04, 0x00};
    for (int i = 0; i < 6; i++)
        lisa_profile_porta_write(pf, cmd[i], true); // handshaked register
}

// Full read: 1st handshake ($01), command, 2nd handshake ($02), stream `n` bytes.
static void read_block(lisa_profile_t *pf, uint32_t block, uint8_t *out, int n) {
    ASSERT_EQ_INT(handshake(pf, 0x55), 0x01);
    send_command(pf, 0x00 /*read*/, block);
    ASSERT_EQ_INT(handshake(pf, 0x55), 0x02);
    for (int i = 0; i < n; i++)
        out[i] = lisa_profile_porta_read(pf, true);
}

// Full write: 1st handshake ($01), command, 2nd handshake ($03), 532 data bytes,
// completion handshake ($06), 4 status bytes.
static void write_block(lisa_profile_t *pf, uint32_t block, const uint8_t *data) {
    ASSERT_EQ_INT(handshake(pf, 0x55), 0x01);
    send_command(pf, 0x01 /*write*/, block);
    ASSERT_EQ_INT(handshake(pf, 0x55), 0x03);
    for (int i = 0; i < PRO_BLOCK; i++)
        lisa_profile_porta_write(pf, data[i], true);
    ASSERT_EQ_INT(handshake(pf, 0x55), 0x06);
    for (int i = 0; i < 4; i++)
        (void)lisa_profile_porta_read(pf, true); // drain status
}

// ============================================================================

TEST(test_attach_detection) {
    lisa_profile_t *pf = lisa_profile_init(bsy_cb, NULL, NULL);
    ASSERT_TRUE(pf != NULL);
    ASSERT_TRUE(!lisa_profile_connected(pf)); // nothing attached yet
    ASSERT_TRUE(lisa_profile_attach(pf, NULL, true)); // blank in-memory image
    ASSERT_TRUE(lisa_profile_connected(pf));
    ASSERT_TRUE(lisa_profile_attached(pf));
    lisa_profile_detach(pf);
    ASSERT_TRUE(!lisa_profile_connected(pf));
    lisa_profile_delete(pf);
}

TEST(test_device_info_block) {
    lisa_profile_t *pf = lisa_profile_init(bsy_cb, NULL, NULL);
    ASSERT_TRUE(lisa_profile_attach(pf, NULL, true));

    uint8_t buf[PRO_RDLEN];
    read_block(pf, 0xFFFFFF, buf, PRO_RDLEN); // device-info / spare-table block

    // 4 status bytes all OK.
    for (int i = 0; i < 4; i++)
        ASSERT_EQ_INT(buf[i], 0);
    const uint8_t *info = &buf[4]; // controller info follows the status bytes
    ASSERT_TRUE(memcmp(info, "PROFILE", 7) == 0); // device name
    ASSERT_EQ_INT(info[14], 0); // drive type 0 = ProFile (the OS reads offset 14)
    uint32_t blocks = ((uint32_t)info[18] << 16) | ((uint32_t)info[19] << 8) | info[20];
    ASSERT_EQ_INT(blocks, 9728); // 5 MB ProFile
    uint16_t bpb = ((uint16_t)info[21] << 8) | info[22];
    ASSERT_EQ_INT(bpb, PRO_BLOCK); // 532 bytes per block
    lisa_profile_delete(pf);
}

TEST(test_write_read_roundtrip) {
    lisa_profile_t *pf = lisa_profile_init(bsy_cb, NULL, NULL);
    ASSERT_TRUE(lisa_profile_attach(pf, NULL, true));

    // A distinctive 532-byte block (20-byte tag + 512 data).
    uint8_t out[PRO_BLOCK];
    for (int i = 0; i < PRO_BLOCK; i++)
        out[i] = (uint8_t)(i * 7 + 3);
    write_block(pf, 42, out);

    uint8_t in[PRO_RDLEN];
    read_block(pf, 42, in, PRO_RDLEN);
    for (int i = 0; i < 4; i++)
        ASSERT_EQ_INT(in[i], 0); // status OK
    ASSERT_TRUE(memcmp(&in[4], out, PRO_BLOCK) == 0); // exact round-trip

    // A different block is still blank — no cross-contamination.
    uint8_t other[PRO_RDLEN];
    read_block(pf, 43, other, PRO_RDLEN);
    for (int i = 4; i < PRO_RDLEN; i++)
        ASSERT_EQ_INT(other[i], 0);
    lisa_profile_delete(pf);
}

TEST(test_decline_aborts) {
    // A handshake reply other than $55 (e.g. the boot ROM's $00 "probe") must
    // abort the transaction, leaving the controller idle for the next command.
    lisa_profile_t *pf = lisa_profile_init(bsy_cb, NULL, NULL);
    ASSERT_TRUE(lisa_profile_attach(pf, NULL, true));
    ASSERT_EQ_INT(handshake(pf, 0x00), 0x01); // probe: present $01, decline
    // A following real read still works from a clean state.
    uint8_t buf[PRO_RDLEN];
    read_block(pf, 0xFFFFFF, buf, PRO_RDLEN);
    ASSERT_TRUE(memcmp(&buf[4], "PROFILE", 7) == 0);
    lisa_profile_delete(pf);
}

int main(void) {
    RUN(test_attach_detection);
    RUN(test_device_info_block);
    RUN(test_write_read_roundtrip);
    RUN(test_decline_aborts);
    printf("[PASS] All ProFile tests passed\n");
    return 0;
}
