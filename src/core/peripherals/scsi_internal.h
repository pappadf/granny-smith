// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// scsi_internal.h
// Shared internals for the SCSI subsystem (controller + device modules).

#ifndef SCSI_INTERNAL_H
#define SCSI_INTERNAL_H

#include "common.h"
#include "image.h"
#include "memory.h"
#include "scsi.h"
#include "via.h"

#include <stdbool.h>
#include <stdint.h>

// ============================================================================
// Constants and Macros
// ============================================================================

// Register offsets (NCR 5380, active-low accent notation from [5])
#define CDR   0 // current scsi data register
#define ODR   0 // output data register
#define ICR   1 // initiator command register
#define MR    2 // mode register
#define TCR   3 // target command register
#define CSR   4 // current scsi bus status register
#define SER   4 // select enable register
#define BSR   5 // bus and status register
#define DMA   5 // start dma send
#define TDMA  6 // start dma target receive
#define IDR   6 // input data register
#define IDMA  7 // start dma initiator receive
#define RESET 7 // reset parity/interrupt

// Initiator command register bits
#define ICR_DB  0x01 // assert data bus
#define ICR_ATN 0x02 // assert ATN
#define ICR_SEL 0x04
#define ICR_BSY 0x08
#define ICR_ACK 0x10
#define ICR_LA  0x20
#define ICR_AIP 0x40
#define ICR_RST 0x80

// Mode register bits
#define MR_ARBITRATE 0x01
#define MR_DMA       0x02
#define MR_TARGET    0x40 // target mode

// Target command register bits
#define TCR_CD 0x02

// Current SCSI bus status register bits
#define CSR_SEL 0x02
#define CSR_IO  0x04
#define CSR_CD  0x08
#define CSR_MSG 0x10
#define CSR_REQ 0x20
#define CSR_BSY 0x40
#define CSR_RST 0x80

// Bus and status register bits (NCR 5380/53C80 BSR, read-only register 5)
#define BSR_ACK  0x01 // bit 0: ACK sensed on bus
#define BSR_ATN  0x02 // bit 1: ATN sensed on bus
#define BSR_PM   0x08 // bit 3: phase match (bus phase matches TCR)
#define BSR_INT  0x10 // bit 4: interrupt request active (/IRQ asserted)
#define BSR_DR   0x40 // bit 6: DMA request (data ready for DMA transfer)
#define BSR_EDMA 0x80 // bit 7: end of DMA

// SCSI command opcodes
#define CMD_TEST_UNIT_READY  0x00
#define CMD_REZERO_UNIT      0x01
#define CMD_REQUEST_SENSE    0x03
#define CMD_FORMAT_UNIT      0x04
#define CMD_READ             0x08
#define CMD_WRITE            0x0A
#define CMD_SEEK_6           0x0B
#define CMD_INQUIRY          0x12
#define CMD_MODE_SELECT      0x15
#define CMD_RESERVE          0x16
#define CMD_RELEASE          0x17
#define CMD_MODE_SENSE       0x1A
#define CMD_START_STOP_UNIT  0x1B
#define CMD_SEND_DIAGNOSTIC  0x1D
#define CMD_PREVENT_ALLOW    0x1E
#define CMD_READ_CAPACITY    0x25
#define CMD_READ_10          0x28
#define CMD_WRITE_10         0x2A
#define CMD_SEEK_10          0x2B
#define CMD_WRITE_VERIFY     0x2E
#define CMD_VERIFY           0x2F
#define CMD_READ_SUB_CHANNEL 0x42
#define CMD_READ_TOC         0x43
#define CMD_READ_HEADER      0x44
#define CMD_PLAY_AUDIO_10    0x45
#define CMD_PLAY_AUDIO_MSF   0x47
#define CMD_PAUSE_RESUME     0x4B

// Sony vendor commands (CDU-8002 proprietary)
#define CMD_SONY_READ_TOC        0xC1
#define CMD_SONY_PLAYBACK_STATUS 0xC4
#define CMD_SONY_PAUSE           0xC5
#define CMD_SONY_PLAY_TRACK      0xC6
#define CMD_SONY_PLAY_MSF        0xC7
#define CMD_SONY_PLAY_AUDIO      0xC8
#define CMD_SONY_PLAYBACK_CTRL   0xC9

// SCSI messages
#define MSG_CMD_COMPLETE 0x00

// SCSI status codes
#define STATUS_GOOD            0x00
#define STATUS_CHECK_CONDITION 0x02

// Sense keys
#define SENSE_NO_SENSE        0x00
#define SENSE_NOT_READY       0x02
#define SENSE_MEDIUM_ERROR    0x03
#define SENSE_ILLEGAL_REQUEST 0x05
#define SENSE_UNIT_ATTENTION  0x06
#define SENSE_DATA_PROTECT    0x07

// Additional sense codes (ASC)
#define ASC_NO_ASC               0x00
#define ASC_WRITE_FAULT          0x03
#define ASC_INVALID_OPCODE       0x20
#define ASC_LBA_OUT_OF_RANGE     0x21
#define ASC_INVALID_FIELD_IN_CDB 0x24
#define ASC_WRITE_PROTECTED      0x27
#define ASC_NOT_READY_TO_READY   0x28
// "Power on, reset or BUS DEVICE RESET occurred" -- the third of the three
// codes the CDU-541 manual lists under UNIT ATTENTION (6h), and what a bus
// reset raises on every target (ANSI X3.131-1986 S6.1.3).
#define ASC_POWER_ON_OR_RESET  0x29
#define ASC_MEDIUM_NOT_PRESENT 0x3A
// The drive we advertise is a SONY CD-ROM CDU-8002 (system.c), so its sense
// vocabulary is the CDU-541 manual's, not SCSI-2's.  That manual's NOT READY
// (2h) table has no 0x3A at all -- an empty bay is vendor code 0xB0, "Caddy not
// inserted in drive" (CDU-541 SCSI manual, sense code tables).  Apple's CD-ROM
// driver was written against these drives, so 0xB0 is what it expects to see.
#define ASC_SONY_CADDY_NOT_INSERTED 0xB0
// Refusing an eject because PREVENT MEDIUM REMOVAL is latched.  CDU-541 manual
// S5.2.33: "the sense key will be set to ILLEGAL REQUEST, and the additional
// sense code set to PREVENT BIT SET", which its ILLEGAL REQUEST (5h) table
// numbers 0x80.  SCSI-2's 0x53/0x02 MEDIUM REMOVAL PREVENTED is a different
// vocabulary and does not appear anywhere in this drive's tables.
#define ASC_SONY_PREVENT_BIT_SET 0x80
#define ASC_INCOMPATIBLE_MEDIUM  0x30

// Block size and buffer limits
#define BLOCK_SIZE 512
#define BUF_LIMIT  (BLOCK_SIZE * 256)
// Largest CDB cmd_size() can ask for: a group 5 (twelve-byte) command.  This
// only sizes the expected-byte count for the COMMAND phase; buf.data itself is
// a BUF_LIMIT allocation, so the slot costs nothing.
#define MAX_CMD_SIZE 12

// ============================================================================
// Type Definitions
// ============================================================================

// SCSI bus phases are declared in the public scsi.h (external bus
// masters need them for the bus-master helpers).

// SCSI device type (HD vs CD-ROM)
enum scsi_device_type {
    scsi_dev_none = 0,
    scsi_dev_hd,
    scsi_dev_cdrom,
};

// Forward typedef (matches scsi.h)
typedef struct scsi scsi_t;

// SCSI controller state (NCR 5380 emulation)
// Per-slot back-link stored on scsi_t so each device entry object's
// instance_data can recover (scsi, slot) in one indirection.
typedef struct {
    struct scsi *scsi;
    int slot;
} scsi_device_link_t;

typedef struct scsi_5380 scsi_5380_t;

struct scsi {

    /* Plain POD fields first (no pointers) */
    struct {
        scsi_phase_t phase;
        scsi_phase_t saved_phase; // phase before MESSAGE OUT (for return)
        int initiator;
        int target;
        // REQ and BSY are bus signals, not chip state.  They used to be
        // stored only inside the 5380's CSR, which is why the bus wrote that
        // register on every phase change.
        bool req;
        bool bsy;
        // The byte the target is currently presenting on the data lines --
        // the status byte, then the completion message.  Also wire state: the
        // 5380 returns it from CDR, the external-initiator API reads it
        // directly, and it used to be stored in the 5380's register.
        uint8_t data;
    } bus;

    struct { // information about current/pending command
        uint8_t opcode; // opcode
        int lun; // logical unit number
        int lba; // logical block address
        int tl; // transfer length
    } cmd;

    /*
     * First pointer-containing member: devices array (vendor/product
     * strings are plain-data but each device contains an image pointer
     * which is runtime-only). We serialize vendor/product separately.
     */
    struct {
        unsigned char vendor_id[8 + 1];
        unsigned char product_id[16 + 1];
        unsigned char revision[4 + 1];
        enum scsi_device_type type;
        bool read_only;
        uint16_t block_size; // 512 for HD, 2048 for CD-ROM (switchable)
        // What block_size returns to on a hard RESET.  MODE SELECT can change
        // the live one at runtime -- A/UX switches the CD-ROM to 512-byte
        // blocks that way -- and ANSI X3.131-1986 S5.2.2.1 requires a reset to
        // "Return any SCSI device operating modes (MODE SELECT, PREVENT/ALLOW
        // MEDIUM REMOVAL commands, etc) to their default conditions".
        uint16_t default_block_size;
        bool unit_attention; // pending UNIT ATTENTION
        bool medium_present; // true when disc is loaded
        bool prevent_removal; // PREVENT/ALLOW MEDIUM REMOVAL state
        struct {
            uint8_t key; // sense key
            uint8_t asc; // additional sense code
            uint8_t ascq; // additional sense code qualifier
        } sense;
    } devices[8];

    // A loopback/terminator card fitted to the bus.  A property of the WIRE --
    // a card is plugged in or it is not -- even though the only thing that can
    // observe it is a chip reading its own data register.  Last field of the
    // plain-data block, so it rides the single checkpoint write with the rest.
    bool loopback;

    /* Buffer metadata and pointer (data is a pointer, so placed after POD fields)
     * Note: max/size are part of the non-pointer metadata but the struct contains
     * a pointer, so buf is placed after the POD region and handled separately.
     */
    struct { // buffer to hold incoming/outgoing data
        uint8_t *data; // byte array
        size_t max; // expected byte count for the active phase
        size_t size; // bytes currently staged (data-out) / remaining (data-in)
        size_t cap; // allocated capacity of `data` (>= BUF_LIMIT, grows on demand)
        size_t pos; // data-in read cursor: index of the next byte to deliver
    } buf;

    // The NCR 5380 driving this bus, or NULL on the machines that have none.
    // The Quadras, the AVs, the PowerMacs and the Network Servers all used to
    // carry a full 5380 register file inside this struct and never touch it,
    // because the chip and the wire were one allocation.
    //
    // This is the bus knowing what is attached to it, which is the direction
    // the dependency should run.  The other three controllers need no such
    // pointer: they are pure clients, driving the bus through scsi.h and
    // reading it through scsi_get_bus_phase().
    // ---- NOT saved -------------------------------------------------------

    // The medium in each slot.  Held out here rather than inside devices[] so
    // that array stays pure plain data and rides in the single block above --
    // a pointer in the middle of it is what forced this file's checkpoint to
    // be a hand-written per-field loop, and what let fields be forgotten.
    // The filename is saved separately and the image re-opened on restore.
    image_t *device_images[8];

    // An armed selection time-out, if a controller is waiting on one.
    //
    // Below the plain-data line deliberately: fn is a host function pointer and
    // ctx a host address, so neither may be written to a checkpoint, and
    // seltmo_registered names a scheduler registration belonging to THIS
    // process.  A restore therefore lands with nothing armed -- the contract is
    // spelled out on scsi_bus_arm_select_timeout() in scsi.h.
    scsi_select_timeout_fn seltmo_fn;
    void *seltmo_ctx;
    bool seltmo_registered;

    scsi_5380_t *chip5380;

    struct object *object; // top-level scsi node
    struct object *bus_object; // scsi.bus child
    struct object *devices_object; // scsi.device collection
    struct object *device_objects[8]; // per-slot entry objects
    struct object *image_objects[8]; // per-slot medium (image) nodes — device[N].image
    // Per-slot back-link used as instance_data on each device entry
    // object so accessors can recover (scsi, slot) cheaply.
    scsi_device_link_t device_links[8];
};

// ============================================================================
// The NCR 5380
// ============================================================================
//
// A controller attached to a bus, exactly like the 53C96, the 53C825 SCRIPTS
// engine and MESH.  It used to BE the bus: this register file and all of the
// pin, DMA and priming state below lived inside struct scsi.
struct scsi_5380 {
    // ---- plain data, saved as one block ----------------------------------
    // Everything up to the first pointer is written in a single
    // system_write_checkpoint_data() call, the same shape via_t, scc_t and
    // rtc_t use.  Adding a field here is enough to make it survive a restore;
    // adding one below the line is a deliberate statement that it should not.
    struct {
        uint8_t cdr;
        uint8_t odr;
        uint8_t icr;
        uint8_t mr;
        uint8_t tcr;
        uint8_t csr;
        uint8_t ser;
        uint8_t bsr;
    } reg;

    // Tracked output pin states (active-low: true = asserted = pin driven low)
    bool irq_active;
    bool drq_active;
    // Buffer size at the last DRQ re-pulse; used to detect the host beginning
    // to drain a block so the re-pulse stops (one wake per block).
    size_t drq_pulse_last_size;
    // Internal end-of-DMA flag (phase changed while DMA active)
    bool end_of_dma;
    bool dma_write_armed;
    uint8_t primer_byte; // value of the held first byte
    uint32_t primer_pc; // PC at which the held byte was written
    bool primer_held; // true while a held first byte awaits decision
    bool dma_out_engine_started;
    uint8_t cdr_pipeline[3];
    int cdr_idx;

    // ---- NOT saved -------------------------------------------------------
    // Deliberately below the line rather than merely omitted.
    //
    // drq_evt_registered records that this PROCESS registered the DRQ service
    // event type with its scheduler.  Restoring it as true would make a fresh
    // process skip the registration and lose the event entirely, so it must
    // start false and be re-established by the first schedule.
    bool drq_evt_registered;

    // Runtime pointers: re-bound by the machine after a restore.
    scsi_t *bus;
    memory_map_t *memory_map;
    memory_interface_t memory_interface;
    via_t *via;
    scsi_irq_fn irq_cb;
    void *irq_cb_ctx;
};

// ============================================================================
// The seam between the bus (scsi_bus.c) and the NCR 5380 (scsi.c)
// ============================================================================
//
// These two lists ARE the coupling, written down so it can be seen and reduced.
// Every other controller -- the 53C96, the 53C825 SCRIPTS engine, MESH -- needs
// none of the first list: they drive the bus through the initiator API in
// scsi.h (select / push / pop / status / message / release) and read phase
// through scsi_get_bus_phase().  The 5380 needs twelve bus internals because it
// grew up inside the bus's own translation unit rather than as a client of it.
//
// Narrowing the first list is the measure of progress on that.

// Bus internals the 5380 still reaches for.
int cmd_size(uint8_t opcode);
void command_complete(scsi_t *scsi);
uint8_t next_byte(scsi_t *scsi);
void phase_arbitration(scsi_t *scsi);
void phase_command(scsi_t *scsi);
void phase_free(scsi_t *scsi);
void phase_message_out(scsi_t *scsi);
const char *phase_name(int p);
void phase_selection(scsi_t *scsi);
void run_cmd(scsi_t *scsi);
void scsi_buf_ensure(scsi_t *scsi, size_t bytes);

// 5380 services the bus calls back into.  Three of these are the chip's
// interrupt and DRQ wiring, which the bus pokes when a phase changes; the
// fourth is the pseudo-DMA byte path.  A bus that did not know which chip was
// attached would not need any of them -- see the notes in scsi_bus.c.
void scsi_cancel_drq_service(scsi_t *scsi);
void scsi_odr_auto_handshake_byte(scsi_t *scsi, uint8_t value, bool apply_primer_gate);
void scsi_update_drq(scsi_t *scsi);
void scsi_update_irq(scsi_t *scsi);
bool scsi_5380_dma_mode(const scsi_t *scsi);
void scsi_5380_entered_status(scsi_t *scsi, bool from_data_in);
void scsi_5380_entered_data_out(scsi_t *bus);
void scsi_5380_bus_freed(scsi_t *bus);
void scsi_5380_dma_push_byte(scsi_t *bus, uint8_t byte);
void scsi_bus_accept_data_out_byte(scsi_t *scsi, uint8_t value);

// ============================================================================
// Phase Transition Helpers (defined in scsi_bus.c, used by scsi_cdrom.c)
// ============================================================================

// Transition SCSI bus to data-in phase (target to initiator)
// Phase names, indexed by scsi_phase_t.  Defined in scsi_bus.c; the object
// model in scsi.c renders the enum from the same table.
extern const char *const SCSI_PHASE_NAMES[];

void phase_data_in(scsi_t *scsi, int bytes);

// Transition SCSI bus to data-out phase (initiator to target)
void phase_data_out(scsi_t *scsi, int bytes);

// Transition SCSI bus to status phase
void phase_status(scsi_t *scsi, uint8_t status);

// Transition SCSI bus to message-in phase
void phase_message_in(scsi_t *scsi, uint8_t message);

// Arm a DATA IN phase for a response of `have` bytes against the allocation
// length `alloc` the CDB carried.  Returns the number of bytes armed; 0 means
// the bus went straight to STATUS GOOD and there is no buffer for the caller to
// fill.
//
// AUTHORITY: an allocation length is a ceiling, never a request.  ANSI
// X3.131-1986 says so once per command -- "the target shall terminate the DATA
// IN phase when allocation length bytes have been transferred or when all
// available data have been transferred to the initiator, whichever is less" --
// and the Sony CDU-541 manual S4.2.6 states it once for every CDB that carries
// one, in the section describing "the common parts of the CDB":
//
//   "An allocation length of zero indicates that no sense data will be
//    transferred.  This condition will not be considered as an error."
//
// So zero means zero.  It is a legal probe, not a cue to send the whole
// response (what five CD-ROM handlers used to do) and not a cue to substitute a
// default (INQUIRY substituted 36, REQUEST SENSE 18).  ANSI's REQUEST SENSE
// section is the one place that names a non-zero answer for a zero allocation
// -- "four bytes of sense data shall be transferred" -- but those four bytes
// are the NONEXTENDED sense format (Table 7-4), which this model does not
// implement: S7.1.2's implementors note frames it as how a target supporting
// both formats picks between them.  Returning four bytes of our extended ($70)
// block would be a truncated header, not that format, so zero is both the more
// faithful answer and the one the drive we advertise documents.
int scsi_data_in_alloc(scsi_t *scsi, int have, int alloc);

// ============================================================================
// CD-ROM Device Functions (defined in scsi_cdrom.c, called from scsi.c)
// ============================================================================

// Handle MODE SENSE(6) for CD-ROM device
void scsi_cdrom_mode_sense(scsi_t *scsi);

// Handle MODE SELECT(6) for CD-ROM device
void scsi_cdrom_mode_select(scsi_t *scsi);

// Handle REQUEST SENSE for any device
void scsi_cdrom_request_sense(scsi_t *scsi);

// Handle READ TOC command
void scsi_cdrom_read_toc(scsi_t *scsi);

// Sony vendor READ TOC (C1h) — CDU-541 TOC Data Format (6-byte descriptors).
void scsi_cdrom_read_toc_sony(scsi_t *scsi);

// Handle READ SUB-CHANNEL command
void scsi_cdrom_read_sub_channel(scsi_t *scsi);

// Handle READ HEADER command
void scsi_cdrom_read_header(scsi_t *scsi);

// Handle START/STOP UNIT command
void scsi_cdrom_start_stop_unit(scsi_t *scsi);

// Handle PREVENT/ALLOW MEDIUM REMOVAL command
void scsi_cdrom_prevent_allow(scsi_t *scsi);

// Set sense data for a device (used by scsi.c for shared error paths)
void scsi_set_sense(scsi_t *scsi, int target, uint8_t key, uint8_t asc, uint8_t ascq);

// Return CHECK CONDITION with sense data already set
void scsi_check_condition(scsi_t *scsi, uint8_t sense_key, uint8_t asc, uint8_t ascq);

#endif // SCSI_INTERNAL_H
