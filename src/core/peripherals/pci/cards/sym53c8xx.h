// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// sym53c8xx.h
// Shared state and register map for the Symbios/LSI 53C8xx family of
// PCI-to-SCSI I/O processors, split between two translation units:
//
//   sym53c825.c      the CHIP — PCI identity, the BAR windows, the
//                    operating register file's non-plain behaviour
//   scripts53c8xx.c  the ENGINE — the SCRIPTS instruction set
//
// The split is deliberate and follows dbdma's: the engine fetches and
// executes a small instruction set out of HOST memory, so it can be driven
// by a mock bus in a unit suite with no PCI, no machine and no SCSI bus
// underneath it, which is the only affordable way to cover five
// instruction classes and their negative cases.
//
// WHY THIS IS A NEW DEVICE CLASS.  Every SCSI controller in this
// repository so far — the 5380, the 53C94/96, MESH — is register-driven: a
// driver writes a command byte and polls.  A 53C8xx is not.  It fetches
// 8- or 12-byte instructions from memory at DSP, executes them, and
// interrupts only when it must.  A register-level model that does not
// execute SCRIPTS moves exactly zero bytes, which is why this is the
// largest single item in the Network Server work and the whole critical
// path to booting AIX.
//
// Reference: Symbios Logic, "PCI-SCSI I/O Processors Programming Guide",
// v2.1 (the architecture and all five instruction classes); LSI Logic,
// "LSI53C825A/825AE PCI to SCSI I/O Processor Technical Manual", v3.1
// (2001), Chapters 4-6 (this exact part).

#ifndef PCI_SYM53C8XX_H
#define PCI_SYM53C8XX_H

#include "memory.h" // memory_interface_t (the BAR backings)

#include <stdbool.h>
#include <stdint.h>

struct config;
struct checkpoint;
struct pci_device;
struct scsi;
typedef struct config config_t;
typedef struct checkpoint checkpoint_t;

// === Geometry ===============================================================
#define SYM825_REGS        128 // implemented operating registers (0x00-0x7F)
#define SYM825_SCRIPTS_RAM 4096u // the internal SCRIPTS RAM behind BAR 2

// === Operating register offsets =============================================
// Little-endian within the chip: a multi-byte register's LOW byte is at the
// LOW offset, which is why writing DSP+3 (its most significant byte) is
// what starts execution.
#define SYM825_SCNTL0 0x00u // full arbitration, start sequence, target mode
#define SYM825_SCNTL1 0x01u
#define SYM825_SCNTL2 0x02u
#define SYM825_SCNTL3 0x03u // clock conversion / synchronous divisors
#define SYM825_SCID   0x04u // this chip's SCSI ID + selection response enables
#define SYM825_SXFER  0x05u // synchronous transfer period and offset
#define SYM825_SDID   0x06u // destination SCSI ID
#define SYM825_GPREG  0x07u
#define SYM825_SFBR   0x08u // SCSI First Byte Received — the ALU's accumulator
#define SYM825_SOCL   0x09u
#define SYM825_SSID   0x0Au
#define SYM825_SBCL   0x0Bu // live SCSI bus control lines (phase, REQ, ACK)
#define SYM825_DSTAT  0x0Cu // DMA status — READ TO CLEAR
#define SYM825_SSTAT0 0x0Du
#define SYM825_SSTAT1 0x0Eu
#define SYM825_SSTAT2 0x0Fu
#define SYM825_DSA    0x10u // Data Structure Address (table-indirect base)
#define SYM825_ISTAT                                                                                                   \
    0x14u // interrupt summary — the only slave-accessible
          // register while SCRIPTS are running
#define SYM825_CTEST0   0x18u
#define SYM825_CTEST1   0x19u
#define SYM825_CTEST2   0x1Au
#define SYM825_CTEST3   0x1Bu // upper nibble = chip revision level
#define SYM825_TEMP     0x1Cu // the CALL/RETURN link register
#define SYM825_DFIFO    0x20u
#define SYM825_CTEST4   0x21u
#define SYM825_CTEST5   0x22u
#define SYM825_CTEST6   0x23u
#define SYM825_DBC      0x24u // 24-bit byte count; DCMD shares the dword
#define SYM825_DCMD     0x27u // the opcode byte of the current instruction
#define SYM825_DNAD     0x28u // next data address
#define SYM825_DSP      0x2Cu // SCRIPTS pointer — writing +3 starts execution
#define SYM825_DSPS     0x30u // the second dword of the current instruction
#define SYM825_SCRATCHA 0x34u
#define SYM825_DMODE    0x38u
#define SYM825_DIEN     0x39u // DMA interrupt enables (gates DSTAT -> IRQ)
#define SYM825_DWT      0x3Au
#define SYM825_DCNTL    0x3Bu // START DMA, single-step, IRQ disable
#define SYM825_ADDER    0x3Cu
#define SYM825_SIEN0    0x40u
#define SYM825_SIEN1    0x41u
#define SYM825_SIST0    0x42u // SCSI interrupt status 0 — READ TO CLEAR
#define SYM825_SIST1    0x43u // SCSI interrupt status 1 — READ TO CLEAR
#define SYM825_SLPAR    0x44u
#define SYM825_MACNTL   0x46u
#define SYM825_GPCNTL   0x47u
#define SYM825_STIME0   0x48u // selection/reselection time-out
#define SYM825_STIME1   0x49u
#define SYM825_RESPID0  0x4Au
#define SYM825_RESPID1  0x4Bu
#define SYM825_STEST0   0x4Cu
#define SYM825_STEST1   0x4Du
#define SYM825_STEST2   0x4Eu
#define SYM825_STEST3   0x4Fu
#define SYM825_SIDL     0x50u
#define SYM825_SODL     0x54u
#define SYM825_SBDL     0x58u
#define SYM825_SCRATCHB 0x5Cu

// === ISTAT (0x14) ===========================================================
#define SYM825_ISTAT_DIP  0x01u // a DMA-type interrupt is pending (see DSTAT)
#define SYM825_ISTAT_SIP  0x02u // a SCSI-type interrupt is pending (see SIST0/1)
#define SYM825_ISTAT_INTF 0x04u // interrupt-on-the-fly; write 1 to clear
#define SYM825_ISTAT_CON  0x08u // connected to the SCSI bus
#define SYM825_ISTAT_SEM  0x10u
#define SYM825_ISTAT_SIGP 0x20u // the driver's "signal process" doorbell
#define SYM825_ISTAT_SRST 0x40u // software reset (self-clearing)
#define SYM825_ISTAT_ABRT 0x80u // abort the current operation

// === DSTAT (0x0C) — DMA-type causes, read to clear ==========================
#define SYM825_DSTAT_IID  0x01u // illegal instruction detected
#define SYM825_DSTAT_WTD  0x02u // watchdog timeout
#define SYM825_DSTAT_SIR  0x04u // a SCRIPTS INT instruction executed
#define SYM825_DSTAT_SSI  0x08u // single-step interrupt
#define SYM825_DSTAT_ABRT 0x10u // aborted by ISTAT ABRT
#define SYM825_DSTAT_BF   0x20u // bus fault
#define SYM825_DSTAT_MDPE 0x40u // master data parity error
#define SYM825_DSTAT_DFE  0x80u // DMA FIFO empty — a LIVE condition, not a cause

// === SIST0 (0x42) / SIST1 (0x43) — SCSI-type causes, read to clear ==========
#define SYM825_SIST0_PAR 0x01u // SCSI parity error
#define SYM825_SIST0_RST 0x02u // SCSI RST/ received
#define SYM825_SIST0_UDC 0x04u // unexpected disconnect
#define SYM825_SIST0_SGE 0x08u // SCSI gross error
#define SYM825_SIST0_RSL 0x10u // reselected
#define SYM825_SIST0_SEL 0x20u // selected
#define SYM825_SIST0_CMP 0x40u // function complete
#define SYM825_SIST0_MA  0x80u // initiator: PHASE MISMATCH; target: SATN/ active
#define SYM825_SIST1_HTH 0x01u // handshake-to-handshake timer expired
#define SYM825_SIST1_GEN 0x02u // general purpose timer expired
#define SYM825_SIST1_STO 0x04u // selection/reselection time-out

// === DCNTL (0x3B) ===========================================================
#define SYM825_DCNTL_COM  0x01u // 53C700 compatibility off
#define SYM825_DCNTL_IRQD 0x02u // IRQ/ disable
#define SYM825_DCNTL_STD  0x04u // START DMA — status must change synchronously
#define SYM825_DCNTL_IRQM 0x08u
#define SYM825_DCNTL_SSM  0x10u // single-step mode

// === SCNTL0 (0x00) ==========================================================
#define SYM825_SCNTL0_TRG   0x01u // target mode
#define SYM825_SCNTL0_START 0x20u // start sequence (arbitrate + select)

// === SCSI phases, as the chip encodes them (SBCL bits 2:0) ==================
#define SYM825_PHASE_DATA_OUT 0u
#define SYM825_PHASE_DATA_IN  1u
#define SYM825_PHASE_COMMAND  2u
#define SYM825_PHASE_STATUS   3u
#define SYM825_PHASE_MSG_OUT  6u
#define SYM825_PHASE_MSG_IN   7u

// === Chip state =============================================================
typedef struct sym53c8xx {
    struct pci_device *dev; // the seated PCI device (back-pointer)
    config_t *cfg; // the machine, for host-memory access
    int channel; // 0 or 1 — which of the board's two controllers

    // The BIG_LIT/ strap.  FALSE on the Apple Network Server, which the
    // ROM proves three ways (see the endianness note in sym53c825.c); a
    // construction parameter rather than a constant because it is a wiring
    // fact, and the SYM53C825AJ variant is little-endian only.  What it
    // governs is the order the ENGINE assembles an instruction dword in;
    // data payloads are a straight byte copy either way, because "the
    // first byte in from the SCSI bus goes to address 0" in both modes.
    bool big_endian;

    // The GPIO pins as this BOARD wires them.  GPIO[3:0] are inputs at
    // power-up with an internal pull-down, so an unwired part reads zero —
    // and on the Apple Network Server that is fatal, because Open
    // Firmware's own `check-disabled` word is:
    //
    //     : check-disabled  … regs >gpreg xb@ 1 and 0=
    //       if  "disabled" encode-string "status" property  then … ;
    //
    // GPIO0 LOW means "this fast/wide channel is not fitted", the node gets
    // `status "disabled"`, and every later `open` of it fails with
    // `Can't open SCSI host adapter`.  So the board pulls GPIO0 HIGH, and
    // the strap is per-instance rather than a constant because it is a
    // wiring fact, not a property of the part.
    uint8_t gpio_strap;

    uint8_t reg[SYM825_REGS]; // the operating register file
    uint8_t dstat; // latched DMA-type causes (DSTAT's readable half)
    uint8_t sist0, sist1; // latched SCSI-type causes
    bool irq; // the IRQ/ pin as this model currently drives it

    uint8_t script_ram[SYM825_SCRIPTS_RAM];

    // Engine state (scripts53c8xx.c).  Execution is STEPPED, not
    // run-to-completion: the engine yields on wait-for-reselect, on phase
    // mismatch and on any interrupt, because a real driver interleaves
    // with the SCSI bus.
    bool running; // SCRIPTS are executing
    // Parked on a Wait Reselect with no reselection to be had.  DSP points
    // AT the instruction, and the driver's SIGP doorbell is what starts
    // the engine again (see exec_io).
    bool waiting_reselect;
    // Arbitrating for a target that is not answering.  The engine is
    // stopped, nothing is reported yet, and STIME0's programmed period has
    // to pass before the time-out latches in SIST1.
    bool select_timeout_armed;
    bool start_pending; // the engine has been asked to run and has not yet
    bool connected; // a target is selected and the bus is not free
    uint8_t target; // the selected target's SCSI id
    uint8_t phase; // the phase the target is currently presenting
    uint32_t insn_count; // instructions executed since power-on (diagnostics)

    // --- The message conversation, which the shared bus model does not
    // --- carry for an external initiator (the MESH front end owns the
    // --- identical problem and solves it the same way).
    //
    // After a select-with-ATN the TARGET enters MESSAGE OUT to collect the
    // initiator's IDENTIFY, but our bus model goes straight to COMMAND.  So
    // the chip presents a VIRTUAL MESSAGE OUT phase until the script's
    // Block Move has delivered the message, and a virtual MESSAGE IN when
    // it has a reply to give (an SDTR/WDTR answer).  Both die with the
    // connection.
    uint8_t msgout_pending; // a virtual MSG OUT is being presented
    uint8_t mo_buf[16]; // the message bytes the initiator has sent
    uint8_t mo_len;
    uint8_t mi_buf[8]; // the message bytes we are giving back
    uint8_t mi_n, mi_rd;
    uint8_t msgin_taken; // the bus model's MESSAGE IN byte was delivered
    // The target has released the bus and the UNEXPECTED DISCONNECT that
    // reports it is owed to the driver — but not until the script has
    // halted, because the driver reads DCMD to find out WHERE the
    // disconnect landed.  See sym53c8xx_start.
    uint8_t disconnect_pending;
    // Negotiated transfer parameters, per connection.  A fast/wide channel
    // is expected to negotiate, and refusing outright would be a lie about
    // what the hardware does; the emulated bus has no timing, so what is
    // modelled is the CONVERSATION, not the rate.
    uint8_t sync_period, sync_offset;
    uint8_t wide; // 1 = 16-bit transfers agreed

    // The shared bus/target model this channel drives.  NULL until the
    // machine attaches one, which is what makes the engine unit-testable.
    struct scsi *bus;

    memory_interface_t regs_if; // BAR 0 (I/O) and BAR 1 (memory)
    memory_interface_t ram_if; // BAR 2 (SCRIPTS RAM)
} sym53c8xx_t;

// === Lifecycle ==============================================================

sym53c8xx_t *sym53c8xx_new(config_t *cfg, int channel);
void sym53c8xx_delete(sym53c8xx_t *s);
// Power-on / SRST state.  Clears the register file and the engine but NOT
// the SCRIPTS RAM, which is memory the host owns.
void sym53c8xx_chip_reset(sym53c8xx_t *s);
// The driver drove RST/: the bus goes back to its power-on state and the
// chip reports SIST0[RST].
void sym53c8xx_bus_reset(sym53c8xx_t *s);
// ISTAT's ABRT bit: abandon whatever the chip is doing and report it.
void sym53c8xx_abort(sym53c8xx_t *s);
void sym53c8xx_checkpoint_save(sym53c8xx_t *s, checkpoint_t *cp);
void sym53c8xx_checkpoint_restore(sym53c8xx_t *s, checkpoint_t *cp);

// Attach the SCSI bus this channel drives (the machine does this after
// pci_seat_slots; a unit suite leaves it NULL and drives the engine
// against a mock).
void sym53c8xx_attach_bus(sym53c8xx_t *s, struct scsi *bus);

// The chip behind a seated PCI device, or NULL when that device is not a
// 53C8xx.  How the machine finds the two controllers its slot table seated
// without reaching into a card's private state or learning its layout.
sym53c8xx_t *sym53c8xx_from_device(struct pci_device *dev);

// === Engine (scripts53c8xx.c) ===============================================

// One `start()` runs until the script stops itself, and a runaway program
// must not take the emulator with it.  The ceiling is far above any real
// SCRIPT — Open Firmware's probe and AIX's driver both run a few dozen
// instructions per connection — and hitting it raises the chip's own
// watchdog cause rather than spinning.
// How long a SCSI transaction takes, from the driver asking to the chip
// interrupting.  Arbitration, selection, the message and command phases, a
// couple of kilobytes of data at ten megabytes a second, status and
// disconnect: a quarter of a millisecond is what that costs on a real
// fast/wide bus, and it is also comfortably longer than any driver spends
// inside the critical section it started the command from.
//
// Zero is the wrong answer and not by a little.  A completion that lands
// inside the store that launched it re-enters the driver's own interrupt
// handler, and AIX's kernel panics on the lock that catches exactly that.
// A completion that lands twenty microseconds later re-enters it a little
// further along, which panics just the same.  This model does not carry
// disconnect/reselect, so one constant stands in for the target's whole
// share of the transaction; it is a floor on how fast I/O can appear to
// complete, and that floor is the point.
#define SYM825_START_LATENCY_NS 250000ull

#define SYM825_INSN_BUDGET 200000

// Begin (or resume) execution at DSP.  Called from the register file when
// the driver writes DCNTL's START DMA bit or the high byte of DSP.  The
// status registers MUST reflect the outcome by the time this returns: the
// driver's first `while (running)` poll happens immediately afterwards,
// and a model that updates asynchronously hangs it with no diagnostic.
void sym53c8xx_start(sym53c8xx_t *s);

// Recompute the IRQ/ pin from the latched causes and their enables, and
// drive the seated device's PCI interrupt line accordingly.
void sym53c8xx_update_irq(sym53c8xx_t *s);

// Latch a DMA-type or SCSI-type cause and halt the engine if it is fatal.
void sym53c8xx_raise_dma(sym53c8xx_t *s, uint8_t dstat_bits);
void sym53c8xx_raise_scsi(sym53c8xx_t *s, uint8_t sist0_bits, uint8_t sist1_bits);

// Host-memory access, as a bus master.  RAM is moved through the backing
// store directly; anything else goes through the slow path.  Exposed so
// the unit suite can substitute a mock.
uint32_t sym53c8xx_read32(sym53c8xx_t *s, uint32_t phys);
void sym53c8xx_read_block(sym53c8xx_t *s, uint32_t phys, uint8_t *buf, uint32_t len);
void sym53c8xx_write_block(sym53c8xx_t *s, uint32_t phys, const uint8_t *buf, uint32_t len);

#endif // PCI_SYM53C8XX_H
