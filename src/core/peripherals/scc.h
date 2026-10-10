// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// scc.h
// Public interface for Serial Communications Controller (SCC) emulation.

#ifndef SCC_H
#define SCC_H

// === Includes ===
#include "common.h"
#include "memory.h"

// === Forward Declarations ===
struct scheduler;

// === Type Definitions ===
struct scc;
typedef struct scc scc_t;

// Callback for SCC interrupt line changes (per-instance routing)
typedef void (*scc_irq_fn)(void *context, bool active);

// Callback for a frame the guest finished transmitting in SDLC mode on
// channel B, the LocalTalk port: the LLAP header and payload, no CRC.
typedef void (*scc_frame_fn)(void *context, const uint8_t *frame, size_t len);

// The channel inputs a device on the far end of a port can drive.  RR0 reports
// each as the chip does: bit set when the (active-low) pin is asserted.
typedef enum {
    SCC_PIN_DCD = 0, // /DCD -> RR0 bit 3
    SCC_PIN_SYNC, // /SYNC -> RR0 bit 4, in asynchronous mode only
    SCC_PIN_CTS, // /CTS -> RR0 bit 5
} scc_pin_t;

// === Lifecycle (Constructor / Destructor / Checkpoint) ===

// Create an SCC instance with per-instance IRQ callback routing.
// irq_cb: called when the SCC interrupt line changes state
// cb_context: opaque pointer passed to the callback
scc_t *scc_init(memory_map_t *map, struct scheduler *scheduler, scc_irq_fn irq_cb, void *cb_context,
                checkpoint_t *checkpoint);

void scc_delete(scc_t *scc);

void scc_checkpoint(scc_t *restrict scc, checkpoint_t *checkpoint);

// === Operations ===

void scc_clock(scc_t *restrict scc, int n);

// Set the BRG source clock frequencies (Hz) for accurate baud-rate timing.
// pclk_hz: PCLK input (e.g. 7833600 for C8M); rtxc_hz: RTxC input (e.g. 3686400).
void scc_set_clocks(scc_t *restrict scc, uint32_t pclk_hz, uint32_t rtxc_hz);

void scc_reset(scc_t *restrict scc);

void scc_set_dcd_a(scc_t *restrict scc, unsigned char val);

void scc_set_dcd_b(scc_t *restrict scc, unsigned char val);

void scc_dcd(scc_t *restrict scc, unsigned int ch, unsigned int dcd);

int scc_sdlc_send(scc_t *restrict scc, uint8_t *buf, size_t len);

// Route the guest's outgoing LocalTalk frames to `fn` (NULL detaches).  Only
// an SDLC frame on channel B is delivered; anything else the transmitter
// flushes -- async bytes, channel A -- is not a LocalTalk frame.
void scc_set_frame_sink(scc_t *scc, scc_frame_fn fn, void *context);

// A front end that owns channel B's LocalTalk link in place of the chip: an
// I/O processor whose firmware runs LLAP itself (the IIfx and Quadra 900 SCC
// IOP), so the guest's frames never pass through the Z8530's SDLC engine.
// While one is installed, scc_sdlc_send hands the network's frames to
// `rx_frame` instead of the receiver, scc_sdlc_ready asks `ready`, and the
// front end transmits through scc_sdlc_divert_tx.  Like the frame sink it is
// the cable's, not the chip's: a reset keeps it and a checkpoint does not
// carry it (the front end reinstalls it).
typedef struct scc_sdlc_divert {
    bool (*ready)(void *ctx); // its LocalTalk driver is up
    void (*rx_frame)(void *ctx, const uint8_t *frame, size_t len); // network -> front end
} scc_sdlc_divert_t;

// Install `divert` (NULL removes it).
void scc_set_sdlc_divert(scc_t *scc, const scc_sdlc_divert_t *divert, void *ctx);

// The front end puts a frame on the wire: it goes to the frame sink, as a
// frame the guest transmitted through the chip would.
void scc_sdlc_divert_tx(scc_t *scc, const uint8_t *frame, size_t len);

// A front end's serial driver transmits `len` asynchronous bytes on channel
// `ch` (0 = A, 1 = B) without the chip: they reach the capture, the port's
// output file and its device exactly as bytes the chip sent would.
void scc_port_tx_bytes(scc_t *scc, unsigned int ch, const uint8_t *buf, size_t len);

// Send every byte the guest transmits on channel `ch` (0 = A, 1 = B) in
// asynchronous mode to the host file `path`, created or truncated; NULL
// closes it.  An open output is a device on the cable, so the port's
// ready line (scc_set_port_ready_line) goes to its ready level while it is
// open.  False, with nothing changed, when the file cannot be opened.
bool scc_set_output(scc_t *scc, unsigned int ch, const char *path);

// The host file channel `ch`'s output goes to, or NULL when none.
const char *scc_get_output(const scc_t *scc, unsigned int ch);

// How the machine wires a device's ready line into channel `ch`: input
// `pin`, at `ready_level` (asserted or not) when a device is there and
// ready.  Drives the pin to "not ready" now; scc_set_output drives it to
// ready while an output is open.  A port that is not wired this way has
// no handshake the output touches.
void scc_set_port_ready_line(scc_t *scc, unsigned int ch, scc_pin_t pin, bool ready_level);

// Undo scc_set_port_ready_line: the input is no longer driven and reads as
// not asserted, as before the port was wired.
void scc_unwire_port_ready_line(scc_t *scc, unsigned int ch);

// A device on channel `ch` drives input `pin` to `asserted`.  The level is
// the device's, not the chip's, so a channel or chip reset keeps it; RR0
// reflects it (SYNC only in asynchronous mode) and a change raises the
// external/status interrupt when the guest enabled it for that input.
void scc_set_input_pin(scc_t *scc, unsigned int ch, scc_pin_t pin, bool asserted);

// A device on the far end of a serial port (a printer): it hears every byte
// the guest transmits on the channel in asynchronous mode, answers through
// scc_port_rx_byte, and says whether it is ready through
// scc_port_device_ready, which drives the port's wired ready line
// (scc_set_port_ready_line).  Like `output`, it is the cable's, not the
// chip's: a channel reset keeps it and a checkpoint does not carry it.
typedef struct scc_port_device {
    const char *name; // what `machine.scc.<ch>.device` reports
    void (*tx_byte)(void *ctx, uint8_t byte); // guest -> device
} scc_port_device_t;

// Plug `dev` into channel `ch` (replacing any device there); NULL unplugs.
// A device starts not ready.
void scc_attach_port_device(scc_t *scc, unsigned int ch, const scc_port_device_t *dev, void *ctx);

// The device on channel `ch`, or NULL.
const scc_port_device_t *scc_port_device(const scc_t *scc, unsigned int ch);

// The device on channel `ch` is ready (or not): the wired ready line
// follows, as it follows an open `output`.
void scc_port_device_ready(scc_t *scc, unsigned int ch, bool ready);

// Device -> guest: `byte` arrives on channel `ch`'s receiver as if from the
// wire (receive FIFO, Rx Character Available, the receive interrupt).
// False when the FIFO is full (Rx Overrun latched, the byte dropped).
bool scc_port_rx_byte(scc_t *scc, unsigned int ch, uint8_t byte);

// Host -> guest as a terminal on the cable sends it (`receive`): the bytes
// queue host-side and arrive at the receiver one character time apart, at the
// channel's programmed rate.  Returns how many fit in the queue (4 KB per
// channel); without a scheduler they arrive at once.
size_t scc_line_send(scc_t *scc, unsigned int ch, const uint8_t *bytes, size_t len);

// True once channel B is in SDLC mode — the guest's AppleTalk driver is up
// and a frame we originate has somewhere to go.
bool scc_sdlc_ready(const scc_t *restrict scc);

// A DMA engine finished feeding a transmit frame into channel `ch` (0 = A,
// 1 = B): flush it as a Tx underrun/EOM would on real hardware.
void scc_dma_tx_complete(scc_t *restrict scc, unsigned int ch);

// A DMA engine drains up to `n` received bytes from channel `ch`'s FIFO
// (0 = A, 1 = B) into `dst`, with the same status side effects as CPU data
// reads.  Returns the count transferred.
size_t scc_dma_rx(scc_t *restrict scc, unsigned int ch, uint8_t *dst, size_t n);

// Get the memory-mapped I/O interface for machine-level address decode
const memory_interface_t *scc_get_memory_interface(scc_t *scc);

// Enable/disable external loopback (port A TX → port B RX, port B TX → port A RX)
void scc_set_external_loopback(scc_t *scc, bool enabled);

// Query external loopback state
bool scc_get_external_loopback(scc_t *scc);

// === Object-model accessors =================================================
//
// Read-only views used by the `scc` / `scc.a` / `scc.b` object classes.
// `ch` is 0 (A) or 1 (B). Out-of-range channels return false / 0.

uint32_t scc_get_pclk_hz(const scc_t *scc);
uint32_t scc_get_rtxc_hz(const scc_t *scc);

// Channel-level read-out: DCD line, TX-empty, queued RX bytes.
bool scc_channel_dcd(const scc_t *scc, unsigned int ch);
bool scc_channel_tx_empty(const scc_t *scc, unsigned int ch);
unsigned scc_channel_rx_pending(const scc_t *scc, unsigned int ch);
// Bytes the host side is still sending on channel `ch` (`receive`), paced at
// one per character time and not yet in the receive FIFO.
unsigned scc_channel_line_in_pending(const scc_t *scc, unsigned int ch);

// Host-side transmit capture (the mirror of `receive`): every byte the guest
// transmits is held until a driving script drains it, so an emulated serial
// console can be asserted on instead of read out of the `scc` log category.
size_t scc_channel_sent_pending(const scc_t *scc, unsigned int ch);
uint64_t scc_channel_sent_dropped(const scc_t *scc, unsigned int ch);
size_t scc_channel_take_sent(scc_t *scc, unsigned int ch, uint8_t *out, size_t max);

#endif // SCC_H
