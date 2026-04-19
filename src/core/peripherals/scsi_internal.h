// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// scsi_internal.h
// Shared internals for the SCSI subsystem (controller + device modules).

#ifndef SCSI_INTERNAL_H
#define SCSI_INTERNAL_H

#include "common.h"
#include "image.h"
#include "memory.h"
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
#define CMD_MODE_SENSE       0x1A
#define CMD_START_STOP_UNIT  0x1B
#define CMD_PREVENT_ALLOW    0x1E
#define CMD_READ_CAPACITY    0x25
#define CMD_READ_10          0x28
#define CMD_WRITE_10         0x2A
#define CMD_SEEK_10          0x2B
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
#define SENSE_ILLEGAL_REQUEST 0x05
#define SENSE_UNIT_ATTENTION  0x06
#define SENSE_DATA_PROTECT    0x07

// Additional sense codes (ASC)
#define ASC_NO_ASC               0x00
#define ASC_INVALID_OPCODE       0x20
#define ASC_LBA_OUT_OF_RANGE     0x21
#define ASC_INVALID_FIELD_IN_CDB 0x24
#define ASC_WRITE_PROTECTED      0x27
#define ASC_NOT_READY_TO_READY   0x28
#define ASC_MEDIUM_NOT_PRESENT   0x3A
#define ASC_INCOMPATIBLE_MEDIUM  0x30

// Block size and buffer limits
#define BLOCK_SIZE   512
#define BUF_LIMIT    (BLOCK_SIZE * 256)
#define MAX_CMD_SIZE 10

// ============================================================================
// Type Definitions
// ============================================================================

// SCSI bus phases
typedef enum scsi_phase {
    scsi_bus_free = 0,
    scsi_arbitration,
    scsi_selection,
    scsi_reselection,
    scsi_command,
    scsi_data_in,
    scsi_data_out,
    scsi_status,
    scsi_message_in,
    scsi_message_out
} scsi_phase_t;

// SCSI device type (HD vs CD-ROM)
enum scsi_device_type {
    scsi_dev_none = 0,
    scsi_dev_hd,
    scsi_dev_cdrom,
};

// Forward typedef (matches scsi.h)
typedef struct scsi scsi_t;

// SCSI controller state (NCR 5380 emulation)
struct scsi {

    /* Plain POD fields first (no pointers) */
    struct {
        scsi_phase_t phase;
        scsi_phase_t saved_phase; // phase before MESSAGE OUT (for return)
        int initiator;
        int target;
    } bus;

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
        image_t *image;
        enum scsi_device_type type;
        bool read_only;
        uint16_t block_size; // 512 for HD, 2048 for CD-ROM (switchable)
        bool unit_attention; // pending UNIT ATTENTION
        bool medium_present; // true when disc is loaded
        bool prevent_removal; // PREVENT/ALLOW MEDIUM REMOVAL state
        struct {
            uint8_t key; // sense key
            uint8_t asc; // additional sense code
            uint8_t ascq; // additional sense code qualifier
        } sense;
    } devices[8];

    /* Buffer metadata and pointer (data is a pointer, so placed after POD fields)
     * Note: max/size are part of the non-pointer metadata but the struct contains
     * a pointer, so buf is placed after the POD region and handled separately.
     */
    struct { // buffer to hold incoming/outgoing data
        uint8_t *data; // byte array
        size_t max; // max size
        size_t size; // current size
    } buf;

    /* Runtime-only pointers and interfaces last */
    memory_map_t *memory_map;
    memory_interface_t memory_interface;

    // VIA2 for interrupt delivery (SE/30); NULL on Plus
    via_t *via;

    // Tracked output pin states (active-low: true = asserted = pin driven low)
    bool irq_active;
    bool drq_active;

    // Internal end-of-DMA flag (phase changed while DMA active)
    bool end_of_dma;

    // Loopback mode: simulate passive SCSI terminator (test card)
    bool loopback;

    // CDR pipeline delay: the NCR 5380's bus drivers take 2 register-write
    // cycles to propagate, so CDR reads the bus state from before the
    // second-to-last register write.  Modeled as a 3-element ring buffer.
    uint8_t cdr_pipeline[3];
    int cdr_idx;
};

// ============================================================================
// Phase Transition Helpers (defined in scsi.c, used by scsi_cdrom.c)
// ============================================================================

// Transition SCSI bus to data-in phase (target to initiator)
void phase_data_in(scsi_t *scsi, int bytes);

// Transition SCSI bus to data-out phase (initiator to target)
void phase_data_out(scsi_t *scsi, int bytes);

// Transition SCSI bus to status phase
void phase_status(scsi_t *scsi, uint8_t status);

// Transition SCSI bus to message-in phase
void phase_message_in(scsi_t *scsi, uint8_t message);

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
