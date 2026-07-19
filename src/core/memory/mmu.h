// SPDX-License-Identifier: MIT
// mmu.h
// 68030 PMMU (Paged Memory Management Unit) interface.
// Implements lazy-fill TLB using SoA pointer arrays in memory.h.
// Table walks resolve guest translation descriptors to physical addresses.

#ifndef MMU_H
#define MMU_H

#include <stdbool.h>
#include <stdint.h>

// Forward declaration (memory.h has the full definition)
typedef struct page_entry page_entry_t;

// === TC Register field extraction ===
#define TC_ENABLE(tc) ((tc) >> 31)
#define TC_SRE(tc)    (((tc) >> 25) & 1)
#define TC_FCL(tc)    (((tc) >> 24) & 1)
#define TC_PS(tc)     (((tc) >> 20) & 0xF)
#define TC_IS(tc)     (((tc) >> 16) & 0xF)
#define TC_TIA(tc)    (((tc) >> 12) & 0xF)
#define TC_TIB(tc)    (((tc) >> 8) & 0xF)
#define TC_TIC(tc)    (((tc) >> 4) & 0xF)
#define TC_TID(tc)    ((tc) & 0xF)

// === Descriptor type codes ===
#define DESC_DT_INVALID 0 // invalid descriptor
#define DESC_DT_PAGE    1 // page descriptor (early termination OK)
#define DESC_DT_TABLE4  2 // valid 4-byte (short) table descriptor
#define DESC_DT_TABLE8  3 // valid 8-byte (long) table descriptor

// === TT Register field extraction ===
#define TT_ENABLE(tt)  (((tt) >> 15) & 1)
#define TT_BASE(tt)    (((tt) >> 24) & 0xFF)
#define TT_MASK(tt)    (((tt) >> 16) & 0xFF)
#define TT_RW(tt)      (((tt) >> 13) & 1) // 0=match both, 1=match R/W field
#define TT_RWM(tt)     (((tt) >> 12) & 1) // 0=match writes, 1=match reads
#define TT_CI(tt)      (((tt) >> 10) & 1) // cache inhibit
#define TT_FC_BASE(tt) (((tt) >> 4) & 7) // function code base
#define TT_FC_MASK(tt) ((tt) & 7) // function code mask

// === MMUSR (MC68030 PMMU status register) bit positions ===
// Must match the hardware layout exactly: A/UX reads MMUSR after PTEST and
// masks specific bits.  e.g. realvtop ($5A596) tests `MMUSR & $E400` =
// {B(15), L(14), S(13), I(10)} to decide a translation failed; if I sits at
// the wrong bit, an invalid page reads back as a valid translation to whatever
// the (zero) descriptor encodes — physical 0 — and the kernel writes user data
// onto the vector table.  Likewise W must be bit 11 (NOT in the $E400 mask) so
// a write-protected page doesn't spuriously read as a failed translation.
#define MMUSR_B       (1u << 15) // bus error during walk
#define MMUSR_L       (1u << 14) // limit violation
#define MMUSR_S       (1u << 13) // supervisor-only page
#define MMUSR_W       (1u << 11) // write-protected
#define MMUSR_I       (1u << 10) // invalid descriptor
#define MMUSR_T       (1u << 6) // transparent translation
#define MMUSR_M       (1u << 4) // modified
#define MMUSR_N_SHIFT 0 // number of levels traversed (bits 2:0)

// Result of a table walk
typedef struct mmu_walk_result {
    uint32_t physical_addr; // resolved physical page address
    uint32_t page_size_bits; // log2 of effective page size (e.g. 12 = 4KB, 25 = 32MB)
    uint32_t descriptor_addr; // physical address of the last descriptor examined (for PTEST's A-reg output)
    bool valid; // true if walk succeeded
    bool supervisor_only; // S bit from descriptor
    bool write_protected; // W bit from descriptor
    bool modified; // M bit from descriptor
    uint16_t mmusr; // MMUSR bits for PTEST
} mmu_walk_result_t;

// MMU state for 68030 — registers set via PMOVE instructions
typedef struct mmu_state {
    // 68030 MMU registers (set via PMOVE)
    uint64_t crp; // CPU root pointer (64-bit descriptor)
    uint64_t srp; // Supervisor root pointer
    uint32_t tc; // Translation control
    uint32_t tt0; // Transparent translation register 0
    uint32_t tt1; // Transparent translation register 1
    uint16_t mmusr; // MMU status register

    bool enabled; // TC.E bit — is translation active?

    // Host-side state for table walks
    uint8_t *physical_ram; // base of physical RAM buffer
    uint32_t physical_ram_size; // size of installed RAM in bytes (total, or Bank A in two-bank mode)
    uint32_t ram_size_max; // max RAM the controller supports (e.g. 128MB on SE/30)

    // Optional second physical RAM bank (Macintosh IIsi: Bank A soldered at
    // physical 0 holding the video frame buffer at its bottom; Bank B SIMM
    // expansion at physical $04000000 which the OS makes system "low memory").
    // When ram_b_size==0 (all other machines) RAM is a single contiguous bank
    // and the resolvers use the simple `phys < physical_ram_size` path.  When
    // set, RAM is two discontiguous banks, each mirroring within a 64 MB window
    // (the boot ROM sizes each bank by that wrap).
    uint8_t *physical_ram_b; // host base of Bank B (NULL = single-bank machine)
    uint32_t ram_a_size; // Bank A size in bytes (mirror modulus for [0, ram_b_phys_base))
    uint32_t ram_b_phys_base; // physical address where Bank B begins ($04000000)
    uint32_t ram_b_size; // Bank B size in bytes (mirror modulus); 0 = single-bank
    uint32_t ram_b_window; // size of Bank B's mirror window ($04000000 = 64 MB)
    uint8_t *physical_rom; // base of physical ROM buffer
    uint32_t physical_rom_size; // size of ROM in bytes
    uint32_t rom_phys_base; // physical address where ROM region starts
    uint32_t rom_region_end; // end of ROM mirror region (exclusive)

    // Optional VRAM region (SE/30 built-in video at $FE000000)
    uint8_t *physical_vram; // base of VRAM buffer (NULL if none)
    uint32_t physical_vram_size; // size of VRAM in bytes
    uint32_t vram_phys_base; // physical address where VRAM is mapped

    // Optional VROM region (SE/30 video declaration ROM at $FEFFE000)
    uint8_t *physical_vrom; // base of VROM buffer (NULL if none)
    uint32_t physical_vrom_size; // size of VROM in bytes
    uint32_t vrom_phys_base; // physical address where VROM is mapped

    // Alternate physical addresses for page-table-mapped I/O access.
    // On SE/30, logical $FExxxxxx identity-maps via TT to $FExxxxxx
    // but the ROM's page table remaps it to $50Fxxxxx.
    uint32_t vram_phys_alt; // alternate VRAM physical base (0 = none)
    uint32_t vrom_phys_alt; // alternate VROM physical base (0 = none)

    // NuBus bus error range: only unmapped reads in this physical address
    // range generate bus errors.  Outside this range, unmapped TT-mapped
    // reads return 0 silently (as the hardware does for non-NuBus slots).
    uint32_t nubus_berr_start; // first address that can bus error (inclusive)
    uint32_t nubus_berr_end; // last address that can bus error (inclusive)
} mmu_state_t;

// === Lifecycle ===

// Create MMU state with pointers to physical RAM/ROM.
// ram_size_max is the maximum RAM the memory controller supports (bus error above this).
// rom_region_end marks the end of the address range where ROM mirrors repeat.
mmu_state_t *mmu_init(uint8_t *physical_ram, uint32_t ram_size, uint32_t ram_size_max, uint8_t *physical_rom,
                      uint32_t rom_size, uint32_t rom_phys_base, uint32_t rom_region_end);

// Destroy MMU state
void mmu_delete(mmu_state_t *mmu);

// === TLB Management ===

// Invalidate the entire software TLB: zero every cached SoA set and re-key
// the active one to the MMU's current configuration.  The "nuke" path for
// machine reset, checkpoint restore, and ROM reload; guest PMOVE/PFLUSH
// traffic goes through the notification functions below instead.
void mmu_invalidate_tlb(mmu_state_t *mmu);

// PMOVE-completed notifications (cpu_pmmu_general).  The SoA maintains one
// cached set per MMU configuration; these decide whether a register write
// switches sets (TC), kills enabled sets' contents (root/TT rewrite, PFLUSH),
// or taints the live set (FD forms, where hardware keeps ATC residency).
void mmu_tc_written(mmu_state_t *mmu, bool fd, bool was_enabled);
void mmu_root_tt_written(mmu_state_t *mmu, bool fd);
void mmu_pflush(mmu_state_t *mmu);

// SoA set lifecycle, called from memory.c: attach captures the freshly
// allocated base arrays as set 0 (end of memory_map_init); detach releases
// set storage and points the globals back at the base arrays so
// memory_map_delete frees the right allocation (idempotent).
void mmu_soa_attach(void);
void mmu_soa_detach(void);

// Memory-logpoint support across cached sets: zero one page's entries in
// every set (logical logpoint install), or zero every set's contents
// entirely (physical logpoint install — aliases can live in any set).
void mmu_soa_logpoint_zero_page(uint32_t page_index);
void mmu_soa_zero_all_sets(void);

// Record that page p has been written into one of the active set's four SoA
// arrays since it was last zeroed. Callers in the slow path of memory.c use
// this when lazy-installing an identity mapping for an MMU-disabled access.
void tlb_track_page(uint32_t page_index);

// === Address Translation ===

// Handle a TLB miss: perform table walk (or TT check), fill SoA entry.
// Returns true if translation succeeded (SoA entry populated, retry access).
// Returns false if translation failed (invalid descriptor → caller raises bus error).
bool mmu_handle_fault(mmu_state_t *mmu, uint32_t logical_addr, bool write, bool supervisor);

// PLOAD instruction: like mmu_handle_fault, but always performs a fresh table
// walk, bypassing the block-descriptor cache (PLOAD's purpose is to reload
// the ATC from the current guest tables).
bool mmu_pload(mmu_state_t *mmu, uint32_t logical_addr, bool write, bool supervisor);

// Test address translation without faulting (PTEST instruction).
// Performs table walk and returns MMUSR value.
// If desc_addr_out is non-NULL, also returns the physical address of the last
// descriptor examined (what PTEST's optional A-reg output requires).
uint16_t mmu_test_address(mmu_state_t *mmu, uint32_t logical_addr, bool write, bool supervisor,
                          uint32_t *desc_addr_out);

// Check transparent translation registers (TT0/TT1).
// Returns true if address matches a TT register (bypass table walk).
bool mmu_check_tt(mmu_state_t *mmu, uint32_t addr, bool write, bool supervisor);

// Debug-only translation: resolve logical address to physical without side effects.
// Returns the physical address, or logical_addr if translation fails.
uint32_t mmu_translate_debug(mmu_state_t *mmu, uint32_t logical_addr, bool supervisor);

// Like mmu_translate_debug but reports whether the walk yielded a VALID
// translation.  Side-effect-free (no SoA fill).  Writes the physical address
// (or logical_addr on failure / MMU-off / TT match) to *pa_out and returns
// true iff the address is genuinely translatable.  Callers need this to tell a
// real identity mapping (phys==logical, valid) apart from a FAILED walk (which
// mmu_translate_debug also reports as phys==logical) — the latter must fault,
// not be treated as identity.
bool mmu_translate_checked(mmu_state_t *mmu, uint32_t logical_addr, bool supervisor, uint32_t *pa_out);

// Translate `logical_addr` against an arbitrary CRP root rather than the
// current `mmu->crp`.  Used by the test harness to reach a known
// user-process address space (e.g. MAE under A/UX) regardless of which
// process is currently scheduled.  Returns true on success and writes the
// physical address to `*pa_out`; false on translation failure or if
// `crp_root == 0`.  Side-effect-free: temporarily swaps and restores
// `mmu->crp` around the walk; SoA tables are not touched.
bool mmu_translate_with_crp(mmu_state_t *mmu, uint32_t logical_addr, uint64_t crp_root, uint32_t *pa_out);

// Physical memory read for debug commands (P: prefix).
// Bypasses MMU translation and reads directly from physical RAM/ROM/VRAM.
uint8_t mmu_read_physical_uint8(mmu_state_t *mmu, uint32_t phys_addr);
uint16_t mmu_read_physical_uint16(mmu_state_t *mmu, uint32_t phys_addr);
uint32_t mmu_read_physical_uint32(mmu_state_t *mmu, uint32_t phys_addr);

// Physical memory write for debug commands and harness code.  Bypasses the
// CPU SoA, so the write reliably hits the targeted PA regardless of which
// mode (user/supervisor) is currently active.  Returns true on success;
// false if the PA is unmapped or in a read-only region (ROM/VROM).
bool mmu_write_physical_uint8(mmu_state_t *mmu, uint32_t phys_addr, uint8_t value);
bool mmu_write_physical_uint16(mmu_state_t *mmu, uint32_t phys_addr, uint16_t value);
bool mmu_write_physical_uint32(mmu_state_t *mmu, uint32_t phys_addr, uint32_t value);

// Resolve a physical address to a host pointer, or NULL if not host-backed
// (RAM/ROM/VRAM/VROM only — I/O regions return NULL).  Used by the memory
// slow path when a physical-space logpoint suppressed the SoA fill.
uint8_t *mmu_phys_to_host(mmu_state_t *mmu, uint32_t phys_addr);
// True when the physical address is backed by writable host memory.
bool mmu_phys_is_writable(mmu_state_t *mmu, uint32_t phys_addr);

// Register a VRAM region so table walks and TT matches can resolve it
void mmu_register_vram(mmu_state_t *mmu, uint8_t *vram, uint32_t phys_base, uint32_t size);
void mmu_register_vrom(mmu_state_t *mmu, uint8_t *vrom, uint32_t phys_base, uint32_t size);

// Configure a second physical RAM bank (e.g. the Macintosh IIsi's SIMM Bank B
// at physical $04000000).  After this, Bank A is [0, ram_a_size) host-backed by
// `physical_ram` and mirroring within [0, bank_b_phys_base); Bank B is host-
// backed by `bank_b_host` and mirrors `bank_b_size` within
// [bank_b_phys_base, bank_b_phys_base + bank_b_window).  `physical_ram_size` is
// reset to ram_a_size so the single-bank paths never shadow Bank B.
void mmu_set_ram_bank_b(mmu_state_t *mmu, uint32_t ram_a_size, uint8_t *bank_b_host, uint32_t bank_b_phys_base,
                        uint32_t bank_b_size, uint32_t bank_b_window);

// Global MMU state pointer (set by machine init, NULL for 68000 machines)
extern struct mmu_state *g_mmu;

// Last CRP observed while the CPU was in user mode.  Snapshotted by the
// supervisor→user transition in cpu_internal.h's set_sr path.  A/UX swaps
// CRP per process, so this value pins the user process that was most
// recently on the CPU — used by `set-mouse --aux` to translate MAE
// Toolbox globals (MTemp/RawMouse/Mouse) into MAE's address space even
// when the CPU is currently in supervisor mode.  0 if no user-mode entry
// has been observed yet.
extern uint64_t g_last_user_crp;

#endif // MMU_H
