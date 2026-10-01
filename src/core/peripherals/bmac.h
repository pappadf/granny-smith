// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// bmac.h
// BMAC ("BigMac") — the 10BASE-T Ethernet MAC cell inside Heathrow, the
// beige Power Macintosh G3's I/O controller, fed by two DBDMA channels
// (transmit and receive).  Machine-independent like sonic.c: the family
// supplies the DBDMA engine, the channel numbers and the interrupt line.
//
// Scope: the 16-bit register file on 16-byte centres, the status/interrupt-
// disable pair, the transmit and receive blocks, the hash filter, the
// Microwire serial EEPROM behind SROMCSR that holds the station address,
// the 7-wire serial transceiver interface with its link bit (no MII PHY
// answers on MIFCSR), and frames through DBDMA: transmit assembled from
// OUTPUT_MORE/OUTPUT_LAST, padded and given an FCS; receive terminating the
// INPUT descriptor at the frame's end with S6 set and a 2-byte status
// trailer after the FCS.  There is no wire: a frame sent with the
// transceiver or MII loopback bit set comes back through the receive
// filter, any other frame is sent into the void, and nothing else arrives.
//
// Register truth: Apple's AppleBMacEthernet driver (BMacEnetRegisters.h,
// BMacEnetHW.cpp, BMacEnetPrivate.cpp, BMacEnet.cpp; Darwin open source),
// the Linux bmac driver (drivers/net/ethernet/apple/bmac.[ch]) and the
// NetBSD/OpenBSD bm driver (if_bm.c, if_bmreg.h).

#ifndef GS_CORE_PERIPHERALS_BMAC_H
#define GS_CORE_PERIPHERALS_BMAC_H

#include "checkpoint.h"
#include "dbdma.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct bmac bmac_t;

// Cell interrupt (level): STATUS & ~INTDISABLE != 0.
typedef void (*bmac_irq_fn)(void *ctx, bool level);

// The Rev C beige G3's cell id (its device tree's `cell-id`).
#define BMAC_CHIPID_HEATHROW 0xB1u

// Register offsets from the cell base (16-bit, little-endian on the bus).
#define BMAC_XIFC       0x000u // transceiver interface control
#define BMAC_TXFIFOCSR  0x100u
#define BMAC_TXTH       0x110u
#define BMAC_RXFIFOCSR  0x120u
#define BMAC_XCVRIF     0x160u // serial transceiver interface; bit 8 = link (0 = up)
#define BMAC_CHIPID     0x170u
#define BMAC_MIFCSR     0x180u // MII management bit-bang
#define BMAC_SROMCSR    0x190u // serial EEPROM bit-bang
#define BMAC_STATUS     0x200u // read-to-clear
#define BMAC_INTDISABLE 0x210u
#define BMAC_TXRST      0x420u
#define BMAC_TXCFG      0x430u
#define BMAC_RXRST      0x620u
#define BMAC_RXCFG      0x630u
#define BMAC_MADD2      0x660u // station address bytes 4-5
#define BMAC_MADD1      0x670u // bytes 2-3
#define BMAC_MADD0      0x680u // bytes 0-1
#define BMAC_HASH3      0x700u // hash bits 0-15
#define BMAC_HASH0      0x730u // hash bits 48-63

// STATUS / INTDISABLE bits used by the model.
#define BMAC_ST_FRAME_RECEIVED 0x0001u
#define BMAC_ST_RX_OVERFLOW    0x0020u
#define BMAC_ST_FRAME_SENT     0x0100u

// DBDMA device-status lines the cell drives.
#define BMAC_S5_TX_DONE  0x20u // transmit channel: the transmitter is ready (held)
#define BMAC_S6_RX_FRAME 0x40u // receive channel: the descriptor ended a frame

// `mac` is the station address the EEPROM holds (and Open Firmware copies
// into the device tree's `local-mac-address`).
bmac_t *bmac_init(checkpoint_t *cp, const uint8_t mac[6]);
void bmac_delete(bmac_t *b);
void bmac_checkpoint(bmac_t *b, checkpoint_t *cp);

// The cell reset (Heathrow FCR bit 31's pulse): every register to its
// power-on value; the EEPROM keeps its contents.
void bmac_reset(bmac_t *b);

void bmac_set_irq(bmac_t *b, bmac_irq_fn fn, void *ctx);
// Install the transmit and receive ports on `d`'s channels.
void bmac_attach_dbdma(bmac_t *b, dbdma_t *d, int tx_chan, int rx_chan);

// Register file: `off` is the byte offset from the cell base; values are
// the little-endian register value (the bus edge owns the swap).
uint16_t bmac_read(bmac_t *b, uint32_t off);
void bmac_write(bmac_t *b, uint32_t off, uint16_t value);

// The link the transceiver reports (default up).
void bmac_set_link(bmac_t *b, bool up);
bool bmac_link(const bmac_t *b);

// Station address as the EEPROM holds it.
void bmac_get_mac(const bmac_t *b, uint8_t mac[6]);

// Frames the transmitter sent and the receiver handed to DMA.
uint32_t bmac_tx_frames(const bmac_t *b);
uint32_t bmac_rx_frames(const bmac_t *b);

// The last frame the transmitter sent (padded to 60 bytes, no FCS): its
// length, at most `max` bytes copied.  0 before the first.
int bmac_last_tx(const bmac_t *b, uint8_t *buf, int max);

// Offer a frame (destination address first, no FCS) to the receiver, as
// if it had arrived on the wire.  It passes the address filter or is
// dropped; true when it was queued.
bool bmac_receive(bmac_t *b, const uint8_t *frame, int len);

#endif // GS_CORE_PERIPHERALS_BMAC_H
