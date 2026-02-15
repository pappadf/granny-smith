# Memory Subsystem

The memory subsystem provides address decoding and data access for the emulated
CPU. It maps the guest address space onto host memory (RAM/ROM) and device I/O
handlers through a page table, delivering fast inline access for the common case
while supporting device-mapped regions and cross-page accesses.

## Page Table Architecture

The memory map is organized as a flat array of **page entries**, one per 4 KB
page of the guest address space. Each page entry records how accesses to that
page should be handled:

```c
typedef struct page_entry {
    uint8_t *host_base;          // non-NULL: direct host pointer for RAM/ROM
    const memory_interface_t *dev; // non-NULL: device-mapped I/O handler
    void *dev_context;           // opaque context passed to device callbacks
    uint32_t base_addr;          // subtracted from addr before calling device
    bool writable;               // true for RAM, false for ROM and devices
} page_entry_t;
```

The page table is indexed by `addr >> PAGE_SHIFT` (where `PAGE_SHIFT = 12`,
giving 4 KB pages). For a 24-bit address space (Plus), this yields 4,096
entries; for a 32-bit address space (IIcx), up to 1,048,576 entries.

### Address Masking

All memory accesses are masked by `g_address_mask` before page table lookup:

- **Plus (24-bit):** `g_address_mask = 0x00FFFFFF` — upper 8 bits discarded
- **IIcx (32-bit):** `g_address_mask = 0xFFFFFFFF` — full 32-bit addressing

## Access Paths

### Fast Path (Inline)

The most common case — accessing RAM or ROM within a single page — is handled
entirely inline in `memory.h` with no function call overhead:

```c
static inline uint8_t memory_read_uint8(uint32_t addr)
{
    addr &= g_address_mask;
    page_entry_t *pe = &g_page_table[addr >> PAGE_SHIFT];
    if (__builtin_expect(pe->host_base != NULL, 1))
        return LOAD_BE8(pe->host_base + (addr & PAGE_MASK));
    if (pe->dev)
        return pe->dev->read_uint8(pe->dev_context, addr - pe->base_addr);
    return 0;
}
```

For 16-bit and 32-bit reads, the inline accessor additionally checks that the
access does not cross a page boundary (`(addr & PAGE_MASK) <= PAGE_SIZE - N`).
If the check fails, control falls through to the slow path.

Write accessors additionally check `pe->writable` to prevent writes to ROM.

### Slow Path

The slow-path functions (`memory_read_uint16_slow`, etc.) handle two cases:

1. **Device I/O:** The page has a device handler (`pe->dev != NULL`). The
   device's read/write callback is invoked with the address adjusted by
   `pe->base_addr` so the device sees offsets relative to its own address range.

2. **Cross-page access:** A 16-bit or 32-bit access spans two pages. The slow
   path splits it into byte-sized reads/writes across the page boundary.

### Byte Order

The emulated Motorola 68000 is big-endian. The `LOAD_BE*` and `STORE_BE*`
macros use `__builtin_bswap*` intrinsics to convert between the host byte order
(little-endian on x86/ARM) and big-endian storage in the guest memory buffer.

## Page Table Population

### RAM and ROM

During initialization, `memory_map_init()` allocates the flat RAM buffer and
page table, then `populate_ram_rom_pages()` fills entries for:

- **RAM pages:** `host_base` points into the RAM buffer, `writable = true`
- **ROM pages:** `host_base` points into the ROM area (mirrored), `writable = false`

### Device Regions

When a device registers itself via `memory_map_add()`, the corresponding page
table entries are populated with:

- `host_base = NULL` (not direct-mapped)
- `dev` = pointer to the device's `memory_interface_t` (read/write callbacks)
- `dev_context` = opaque device pointer
- `base_addr` = the device's base address (subtracted from absolute addresses
  before invoking callbacks)

### Per-Instance Ownership

Each `memory_map_t` instance stores its own `page_table` and `page_count`.
The global `g_page_table` pointer is set to the active instance's table during
initialization. During checkpoint restore, when a new memory map replaces the
old one, `memory_map_delete()` only clears the globals if the instance being
destroyed owns the currently active page table. This prevents use-after-free
during the teardown/rebuild sequence.

## MMU Integration (IIcx / 68030)

For the Macintosh IIcx, the 68030's built-in PMMU translates logical addresses
to physical addresses. The page table serves as the translation layer:

- When the guest OS writes to MMU registers via `PMOVE` or executes `PFLUSH`,
  the emulator **rebuilds the entire page table** by walking the guest's
  translation tables in emulated RAM.
- During normal execution, memory accesses use the same inline fast path with
  **zero additional overhead** — the translation is baked into the page table.
- For the Plus (no MMU), the page table is populated once at startup and never
  changes.

The MMU interface is defined in `src/core/memory/mmu.h` (currently a stub).

## Key Files

| File | Purpose |
|------|---------|
| `src/core/memory/memory.h` | Page table types, inline accessors, public API |
| `src/core/memory/memory.c` | Page table allocation, population, slow-path handlers |
| `src/core/memory/mmu.h` | 68030 MMU state struct and planned API (stub) |

---

## Hardware Details (Macintosh Plus)

### ROM

The Macintosh Plus uses two 512K-bit (64K × 8) ROM chips, providing a total of 128 KB of ROM storage. Each chip has 8 data pins, handling one half of the 16-bit system data bus:

* The "low" ROM is connected to data lines D0–D7.
* The "high" ROM is connected to data lines D8–D15.

Each ROM chip has 16 address pins, which connect to address lines A1 through A16 of the system bus. Since the Macintosh Plus uses a 16-bit data bus, A0 of the system bus is not used in this connection. In addition, address line A17 from the system bus is also wired to the ROM socket.

In theory, this design allows the ROM sockets to accommodate chips up to 1M-bit (128K × 8). However, in practice, the Macintosh Plus uses A17 as the **output enable** (/OE) signal for both ROM chips. This means that whenever A17 is high, the ROM chips are disabled and do not drive the bus.

The location of the ROM within the overall memory map is controlled by the /ROMCE signal that is provided by the "CAS" PAL (20L8A). Effectively, the ROM normally occupies the address range **0x00400000–0x004FFFFF**, but is also mapped into **0x00000000–0x000FFFFF** when overlay is enabled. Within this region:

* **0x00000–0x1FFFF**: The standard 128 KB ROM is accessible.
* **0x20000–0x3FFFF**: This range is blank, as A17 is high and the ROM is disabled.
* **0x40000–0x5FFFF**: The ROM reappears as an alias of the original, since only A1–A16 are decoded by the ROM chips.
* This pattern continues, with alternating ROM images and gaps, repeating until **0xFFFFF**.

The bootstrap code actually tests this layout, reading two long words from 0x00420000 and 0x00440000 respectively and comparing them. They should not be the same, as 0x00420000 would be the beginning of a "gap", whereas 0x00440000 is the beginning of a new ROM alias image.

