// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// scsi.c
// Implements SCSI controller emulation for the NCR 5380

#define _CRT_SECURE_NO_WARNINGS 1

#include "scsi.h"
#include "platform.h"
#include "shell.h"
#include "system.h"

#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Constants and Macros
// ============================================================================

// #define LOG(...) log_message(LOG_SCSI, 1, __VA_ARGS__)
#define LOG(...)

// register offsets/definitions based on [5]
#define CDR   0 // current scsi data register
#define ODR   0 // output data register
#define ICR   1 // initiator command register
#define MR    2 // mode register
#define TCR   3 // target command register
#define CSR   4 // current scsi bus status register
#define SER   4 // select enable register
#define BSR   5 // bus and status register
#define DMA   5 // start dma send
#define TDMA  6 // sart dma target receive
#define IDR   6 // input data register
#define IDMA  7 // start dma initiator receive
#define RESET 7 // reset parity/interrupt

// initiator command register
#define ICR_SEL 0x04
#define ICR_BSY 0x08
#define ICR_ACK 0x10
#define ICR_LA  0x20
#define ICR_AIP 0x40
#define ICR_RST 0x80

// mode register
#define MR_ARBITRATE 0x01
#define MR_DMA       0x02

// target command register
#define TCR_CD 0x02

// current scsi bus status register
#define CSR_IO  0x04
#define CSR_CD  0x08
#define CSR_MSG 0x10
#define CSR_REQ 0x20
#define CSR_BSY 0x40
#define CSR_RST 0x80

// bus and status register
#define BSR_PM 0x08
#define BSR_DR 0x40

// command opcodes from [6]
#define CMD_TEST_UNIT_READY 0x00
#define CMD_FORMAT_UNIT     0x04
#define CMD_READ            0x08
#define CMD_WRITE           0x0A
#define CMD_INQUIRY         0x12
#define CMD_MODE_SELECT     0x15
#define CMD_READ_CAPACITY   0x25

// messages
#define MSG_CMD_COMPLETE 0x00

// status codes
#define STATUS_GOOD 0x00

#define BLOCK_SIZE   512
#define BUF_LIMIT    (BLOCK_SIZE * 256)
#define MAX_CMD_SIZE 10

// ============================================================================
// Type Definitions
// ============================================================================

// bus phases
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

struct scsi {

    /* Plain POD fields first (no pointers) */
    struct {
        scsi_phase_t phase;
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
        uint8_t opcode; // opode
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
        image_t *image;
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
};

// ============================================================================
// Static Helpers
// ============================================================================

// Determine the length of a SCSI command based on its opcode
static int cmd_size(uint8_t opcode) {
    return opcode < 0x20 ? 6 : 10;
}

// Pop the next byte from the SCSI buffer
static uint8_t next_byte(scsi_t *scsi) {
    assert(scsi->buf.size > 0);

    uint8_t byte = scsi->buf.data[0];
    memmove(scsi->buf.data, scsi->buf.data + 1, --scsi->buf.size);

    return byte;
}

// Transition SCSI bus to the free/idle state
static void phase_free(scsi_t *scsi) {
    scsi->bus.phase = scsi_bus_free;
    scsi->reg.csr = 0;
}

// Transition SCSI bus to arbitration phase
static void phase_arbitration(scsi_t *scsi) {
    assert(scsi->bus.phase == scsi_bus_free);

    scsi->bus.phase = scsi_arbitration;
}

// Transition SCSI bus to selection phase
static void phase_selection(scsi_t *scsi) {
    assert(scsi->bus.phase == scsi_arbitration);

    scsi->bus.phase = scsi_selection;
}

// Transition SCSI bus to command phase
static void phase_command(scsi_t *scsi) {
    assert(scsi->bus.phase == scsi_selection);

    // by not asserting MSG, we indicate that we don't support any messages (other than command complete)
    // i.e. go directly to the command phase
    scsi->reg.csr = CSR_CD + CSR_REQ + CSR_BSY;

    // reset the buffer - will hold the command
    scsi->buf.max = MAX_CMD_SIZE;
    scsi->buf.size = 0;
    scsi->bus.phase = scsi_command;
}

// Transition SCSI bus to data-in phase (target to initiator)
static void phase_data_in(scsi_t *scsi, int bytes) {
    assert(scsi->bus.phase == scsi_command);

    scsi->bus.phase = scsi_data_in;
    scsi->reg.csr = CSR_IO + CSR_REQ + CSR_BSY;
    scsi->buf.size = scsi->buf.max = bytes;
}

// Transition SCSI bus to data-out phase (initiator to target)
static void phase_data_out(scsi_t *scsi, int bytes) {
    assert(scsi->bus.phase == scsi_command);
    assert(bytes <= BUF_LIMIT);

    scsi->bus.phase = scsi_data_out;
    scsi->reg.csr = CSR_REQ + CSR_BSY;
    scsi->buf.max = bytes;
    scsi->buf.size = 0;
}

// Transition SCSI bus to status phase
static void phase_status(scsi_t *scsi, uint8_t status) {
    assert(scsi->bus.phase == scsi_command || scsi->bus.phase == scsi_data_in || scsi->bus.phase == scsi_data_out);

    scsi->bus.phase = scsi_status;
    scsi->reg.csr = CSR_IO + CSR_CD + CSR_REQ + CSR_BSY;
    scsi->reg.cdr = status;
}

// Transition SCSI bus to message-in phase
static void phase_message_in(scsi_t *scsi, uint8_t message) {
    assert(scsi->bus.phase == scsi_status);

    scsi->bus.phase = scsi_message_in;
    scsi->reg.csr = CSR_IO + CSR_CD + CSR_MSG + CSR_REQ + CSR_BSY;
    scsi->reg.cdr = message;
}

// Execute a SCSI command after receiving it from the initiator
static void run_cmd(scsi_t *scsi) {
    scsi->cmd.opcode = scsi->buf.data[0];

    switch (scsi->cmd.opcode) {

    case CMD_TEST_UNIT_READY:
        LOG("command: TEST UNIT READY");
        phase_status(scsi, STATUS_GOOD);
        break;

    case CMD_FORMAT_UNIT:
        LOG("command: FORMAT UNIT");
        phase_status(scsi, STATUS_GOOD);
        break;

    case CMD_READ:
    case CMD_WRITE:

        scsi->cmd.lun = scsi->buf.data[1] >> 5;
        scsi->cmd.lba = scsi->buf.data[1] << 16 & 0x1FFFFF | scsi->buf.data[2] << 8 | scsi->buf.data[3];
        scsi->cmd.tl = scsi->cmd.tl = (scsi->buf.data[4] - 1 & 0xFF) + 1;

        assert((scsi->buf.data[5] & 0x3) == 0); // FLAG = LINK = 0
        assert(scsi->cmd.lun == 0);
        assert(scsi->cmd.tl * BLOCK_SIZE <= BUF_LIMIT);

        if (scsi->cmd.opcode == CMD_WRITE) {
            phase_data_out(scsi, 512 * scsi->cmd.tl);
        } else {
            phase_data_in(scsi, scsi->cmd.tl * BLOCK_SIZE);
            size_t n = disk_read_data(scsi->devices[scsi->bus.target & 7].image, scsi->cmd.lba * 512, scsi->buf.data,
                                      scsi->cmd.tl * 512);
            assert(n == scsi->cmd.tl * 512);
        }
        break;

    case CMD_INQUIRY:

        assert(scsi->buf.data != NULL);
        assert(scsi->devices[scsi->bus.target & 7].image != NULL);

        // [6]: byte 4 is the "allocation length"
        phase_data_in(scsi, scsi->cmd.tl = scsi->buf.data[4]);

        memset(scsi->buf.data, 0, scsi->cmd.tl);

        // SCSI 1 [6] left most of the inquiry data as vendor specific,
        // but SCSI 2 [7] at least defined the first 36 bytes
        if (scsi->cmd.tl >= 36) {
            memcpy(scsi->buf.data + 8, scsi->devices[scsi->bus.target & 7].vendor_id, 8);
            memcpy(scsi->buf.data + 16, scsi->devices[scsi->bus.target & 7].product_id, 16);
        }

        // [7]: additional length = n - 1
        scsi->buf.data[4] = scsi->cmd.tl - 4;

        break;

    case CMD_MODE_SELECT:
        phase_data_out(scsi, scsi->buf.data[4]);
        break;

    case CMD_READ_CAPACITY:

        assert((scsi->buf.data[8] & 1) == 0); // PMI = 0

        image_t *image = scsi->devices[scsi->bus.target & 7].image;
        size_t size = disk_size(image) / BLOCK_SIZE;

        phase_data_in(scsi, 8);
        *(uint32_t *)(scsi->buf.data) = BE32((uint32_t)size - 1);
        *(uint32_t *)(scsi->buf.data + 4) = BE32(BLOCK_SIZE);

        break;

    default:
        assert(0);
        break;
    }
}

// Finalize a SCSI command after data transfer is complete
static void command_complete(scsi_t *scsi) {
    switch (scsi->cmd.opcode) {

    case 0x0A: // write6
    {
        assert(scsi->cmd.tl * 512 == scsi->buf.size);
        size_t device_bytes = disk_size(scsi->devices[scsi->bus.target & 7].image);
        assert(((size_t)scsi->cmd.lba + scsi->cmd.tl) * BLOCK_SIZE <= device_bytes);

        disk_write_data(scsi->devices[scsi->bus.target & 7].image, scsi->cmd.lba * 512, scsi->buf.data,
                        scsi->cmd.tl * 512);

        scsi->buf.max = scsi->buf.size = 0;
    } break;

    default:
        break;
    }

    phase_status(scsi, STATUS_GOOD);
}

// Perform SCSI bus reset
static void scsi_reset(scsi_t *scsi) {
    // TBD
}

// ============================================================================
// Memory Interface
// ============================================================================

// Write to the initiator command register
static void write_icr(scsi_t *scsi, uint8_t val) {
    uint8_t bits_set = val & (val ^ scsi->reg.icr);
    uint8_t bits_cleared = ~val & (val ^ scsi->reg.icr);

    scsi->reg.icr = val;

    if (bits_set & ICR_RST) {
        scsi_reset(scsi);
        return;
    }

    // if BSY is released in the selection phase, than it marks the end of selection
    if (bits_cleared & ICR_BSY && scsi->reg.icr & ICR_SEL) {

        assert(scsi->bus.phase == scsi_selection);

        // let's assume that we have an identified initiator id
        assert(scsi->bus.initiator < 8);

        // ODR will contain the "OR" of target and initiator ID
        scsi->bus.target = platform_ntz32(scsi->reg.odr & ~(1 << scsi->bus.initiator));

        // [6]: target will assert BSY - if no target, the bus will be free again
        if (!scsi->devices[scsi->bus.target & 7].image)
            phase_free(scsi);
        else
            phase_command(scsi);
    }

    // if ACK is reset
    if (bits_cleared & ICR_ACK) {

        if (scsi->bus.phase == scsi_command) {
            assert(scsi->buf.size <= cmd_size(scsi->buf.data[0]));
            if (scsi->buf.size == cmd_size(scsi->buf.data[0]))
                run_cmd(scsi);
            else
                scsi->reg.csr |= CSR_REQ;
        } else if (scsi->bus.phase == scsi_status)
            phase_message_in(scsi, MSG_CMD_COMPLETE);
        else if (scsi->bus.phase == scsi_message_in)
            phase_free(scsi);
        else if (scsi->bus.phase == scsi_data_in) {
            if (scsi->buf.size == 0)
                phase_status(scsi, STATUS_GOOD);
            else
                scsi->reg.csr |= CSR_REQ;
        } else
            assert(0);
    }

    // if ACK is asserted
    if (bits_set & ICR_ACK) {

        // if a command - save the next byte in the buffer
        if (scsi->bus.phase == scsi_command) {
            assert(scsi->buf.size < scsi->buf.max);
            scsi->buf.data[scsi->buf.size++] = scsi->reg.odr;
        } else if (scsi->bus.phase == scsi_status)
            ;
        else if (scsi->bus.phase == scsi_message_in)
            ;
        else
            assert(0);

        // clear REQ
        scsi->reg.csr &= ~CSR_REQ;
    }

    if (bits_set & ICR_SEL)
        phase_selection(scsi);
}

// Write to the mode register
static void write_mr(scsi_t *scsi, uint8_t val) {
    uint8_t bits_set = val & (val ^ scsi->reg.mr);
    uint8_t bits_cleared = ~val & (val ^ scsi->reg.mr);

    scsi->reg.mr = val;

    // The ARBITRATE bit is set to start the arbitration process
    if (bits_set & MR_ARBITRATE) {

        phase_arbitration(scsi);

        // [1]: The results of the arbitration phase may be determined by reading the status bits LA and AIP
        scsi->reg.icr |= ICR_AIP;

        // Let's assume that we always win arbitration.
        // That is, LA is always cleard, and our own ID is always in the data register
        scsi->reg.icr &= ~ICR_LA;
        scsi->reg.cdr = scsi->reg.odr;
        scsi->bus.initiator = platform_ntz32(scsi->reg.odr);
    }

    if (bits_cleared & MR_DMA) {
        scsi->reg.bsr &= ~BSR_DR;
    } else if (bits_set & MR_DMA) {

        assert(scsi->bus.phase == scsi_data_in || scsi->bus.phase == scsi_data_out);

        // if we're reading in data, and there is more in the buffer - then assert request signal
        if (scsi->bus.phase == scsi_data_in && scsi->buf.size != 0)
            scsi->reg.bsr |= BSR_DR;

        // if we're writing out data, and there is room in the buffer - then assert request signal
        if (scsi->bus.phase == scsi_data_out && scsi->buf.size < scsi->buf.max)
            scsi->reg.bsr |= BSR_DR;
    }
}

// Read a byte from SCSI controller register
static uint8_t read_uint8(void *s, uint32_t addr) {
    scsi_t *scsi = (scsi_t *)s;

    // [5]: read operations must be to even addresses; otherwise undefined
    if (addr & 1)
        return 0;

    // [5] : a0-a2  are connected to a4-a6 of the cpu bus
    switch (addr >> 4 & 7) {
    case CDR:
    case IDR: // unclear if we need to make a distinction between CDR/IDR
        if (scsi->bus.phase == scsi_data_in) {
            if (scsi->buf.size != 0)
                scsi->reg.cdr = next_byte(scsi);
            else
                phase_status(scsi, STATUS_GOOD);
        }
        return scsi->reg.cdr;

    case ICR:
        return scsi->reg.icr;

    case MR:
        return scsi->reg.mr;

    case TCR:
        return scsi->reg.tcr;

    case CSR:
        return scsi->reg.csr;

    case BSR:
        return scsi->reg.bsr | BSR_PM;

    case RESET:
        return 0xff;
    }

    assert(0);
    return 0;
}

// Read a 16-bit word from SCSI controller (not supported)
static uint16_t read_uint16(void *scsi, uint32_t addr) {
    assert(0);
    return 0;
}

// Read a 32-bit longword from SCSI controller (not supported)
static uint32_t read_uint32(void *scsi, uint32_t addr) {
    GS_ASSERT(0);
    return 0;
}

// Write a byte to SCSI controller register
static void write_uint8(void *s, uint32_t addr, uint8_t value) {
    scsi_t *scsi = (scsi_t *)s;

    // [5]: write operations must be to odd addresses; otherwise undefined
    assert(addr & 1);

    // [5] : a0-a2  are connected to a4-a6 of the cpu bus
    switch (addr >> 4 & 7) {
    case ODR:
        if (addr & 0x200) {

            assert(scsi->bus.phase == scsi_data_out);
            assert(scsi->buf.size < scsi->buf.max);

            scsi->buf.data[scsi->buf.size++] = value;

            if (scsi->buf.size == scsi->buf.max)
                command_complete(scsi);
        } else
            scsi->reg.odr = value;
        break;

    case ICR:
        write_icr(scsi, value);
        break;

    case MR:
        write_mr(scsi, value);
        break;

    case TCR:
        scsi->reg.tcr = value;
        // boot code seems to read only 256 bytes of block 0 and 1 (not a full block),
        // and then jump directly to "status" by asserting C/D and I/O in TCR
        if (scsi->bus.phase == scsi_data_in && (value & 7) == 3)
            phase_status(scsi, STATUS_GOOD);
        break;

    case SER:
        scsi->reg.ser = value;
        break;

    case DMA:
        scsi->reg.bsr |= BSR_DR;
        break;

    case TDMA:
    case IDMA:
        break;

    default:
        assert(0);
        break;
    }
}

// Write a 16-bit word to SCSI controller (not supported)
static void write_uint16(void *scsi, uint32_t addr, uint16_t value) {
    assert(0);
}

// Write a 32-bit longword to SCSI controller (not supported)
static void write_uint32(void *scsi, uint32_t addr, uint32_t value) {
    assert(0);
}

// ============================================================================
// Lifecycle: Constructor
// ============================================================================

// Add a SCSI device to the bus at the specified SCSI ID
void scsi_add_device(scsi_t *restrict scsi, int scsi_id, const char *vendor, const char *product, image_t *image) {
    assert(scsi_id < 7);

    scsi->devices[scsi_id].image = image;

    // Cast unsigned char[] to char* to avoid warnings
    sprintf((char *)scsi->devices[scsi_id].vendor_id, "%8s", vendor);
    sprintf((char *)scsi->devices[scsi_id].product_id, "%16s", product);
}

// Initialize the SCSI controller and optionally restore from checkpoint
scsi_t *scsi_init(memory_map_t *map, checkpoint_t *checkpoint) {
    scsi_t *scsi = (scsi_t *)malloc(sizeof(scsi_t));
    if (scsi == NULL)
        return NULL;

    memset(scsi, 0, sizeof(scsi_t));

    scsi->memory_interface.read_uint8 = &read_uint8;
    scsi->memory_interface.read_uint16 = &read_uint16;
    scsi->memory_interface.read_uint32 = &read_uint32;

    scsi->memory_interface.write_uint8 = &write_uint8;
    scsi->memory_interface.write_uint16 = &write_uint16;
    scsi->memory_interface.write_uint32 = &write_uint32;

    memory_map_add(map, 0x00500000, 0x00100000, "scsi", &scsi->memory_interface, scsi);

    scsi->bus.phase = scsi_bus_free;

    scsi->buf.data = malloc(BUF_LIMIT);
    scsi->buf.max = MAX_CMD_SIZE;
    scsi->bus.initiator = INT_MAX;

    // If checkpoint provided, restore plain-data portion first
    if (checkpoint) {
        size_t data_size = offsetof(scsi_t, devices);
        system_read_checkpoint_data(checkpoint, scsi, data_size);

        // Restore device vendor/product strings and image filename; resolve image by name
        for (int i = 0; i < 8; i++) {
            // vendor_id and product_id are fixed-size arrays inside devices[i]
            system_read_checkpoint_data(checkpoint, scsi->devices[i].vendor_id, sizeof(scsi->devices[i].vendor_id));
            system_read_checkpoint_data(checkpoint, scsi->devices[i].product_id, sizeof(scsi->devices[i].product_id));
            uint32_t len = 0;
            system_read_checkpoint_data(checkpoint, &len, sizeof(len));
            scsi->devices[i].image = NULL;
            if (len > 0) {
                char *name = (char *)malloc(len);
                if (name) {
                    system_read_checkpoint_data(checkpoint, name, len);
                    image_t *img = setup_get_image_by_filename(name);
                    if (img)
                        scsi->devices[i].image = img;
                    free(name);
                } else {
                    // consume bytes to keep stream aligned
                    char tmp;
                    for (uint32_t k = 0; k < len; ++k)
                        system_read_checkpoint_data(checkpoint, &tmp, 1);
                }
            }
        }

        // Restore buffer contents (if any). The buf.data has been allocated above. Restore its max/size first.
        system_read_checkpoint_data(checkpoint, &scsi->buf.max, sizeof(scsi->buf.max));
        system_read_checkpoint_data(checkpoint, &scsi->buf.size, sizeof(scsi->buf.size));
        if (scsi->buf.size && scsi->buf.data) {
            size_t to_read = scsi->buf.size;
            if (to_read > BUF_LIMIT)
                to_read = BUF_LIMIT;
            system_read_checkpoint_data(checkpoint, scsi->buf.data, to_read);
        }
    }

    return scsi;
}

// ============================================================================
// Lifecycle: Destructor
// ============================================================================

// Free resources associated with SCSI controller
void scsi_delete(scsi_t *scsi) {
    if (!scsi)
        return;
    if (scsi->buf.data) {
        free(scsi->buf.data);
        scsi->buf.data = NULL;
    }
    free(scsi);
}

// ============================================================================
// Lifecycle: Checkpointing
// ============================================================================

// Save SCSI controller state to checkpoint
void scsi_checkpoint(scsi_t *restrict scsi, checkpoint_t *checkpoint) {
    if (!scsi || !checkpoint)
        return;

    // Save contiguous plain-data portion up to devices (devices contains pointers)
    size_t data_size = offsetof(scsi_t, devices);
    system_write_checkpoint_data(checkpoint, scsi, data_size);

    // Save vendor/product strings and per-device image filename (len=0 means no image)
    for (int i = 0; i < 8; i++) {
        system_write_checkpoint_data(checkpoint, scsi->devices[i].vendor_id, sizeof(scsi->devices[i].vendor_id));
        system_write_checkpoint_data(checkpoint, scsi->devices[i].product_id, sizeof(scsi->devices[i].product_id));
        const char *name = scsi->devices[i].image ? image_get_filename(scsi->devices[i].image) : NULL;
        uint32_t len = 0;
        if (name && *name)
            len = (uint32_t)strlen(name) + 1; // include NUL
        system_write_checkpoint_data(checkpoint, &len, sizeof(len));
        if (len) {
            system_write_checkpoint_data(checkpoint, name, len);
        }
    }

    // Save buf metadata and contents
    system_write_checkpoint_data(checkpoint, &scsi->buf.max, sizeof(scsi->buf.max));
    system_write_checkpoint_data(checkpoint, &scsi->buf.size, sizeof(scsi->buf.size));
    if (scsi->buf.size && scsi->buf.data) {
        size_t to_write = scsi->buf.size;
        if (to_write > BUF_LIMIT)
            to_write = BUF_LIMIT;
        system_write_checkpoint_data(checkpoint, scsi->buf.data, to_write);
    }
}
