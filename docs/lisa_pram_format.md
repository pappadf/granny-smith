# Apple Lisa Parameter Memory (PRAM) — byte format

This documents the exact layout of the Lisa's 64-byte **parameter memory** (PRAM):
where it lives, what each byte means, how the device-configuration table is
packed, and how the validity checksum is computed — enough to **synthesize** a
PRAM image by hand (e.g. to make the OS boot from a ProFile hard disk).

It was reverse-engineered from the (authoritative) LisaOS and boot-ROM source
and verified byte-for-byte against two PRAM images captured from
our own emulator (a floppy-only system and a system with a ProFile installed).
See [docs/lisa.md §13.4](lisa.md) for the surrounding controller-RAM context and
[docs/lisa_profile_hd.md](lisa_profile_hd.md) for the ProFile device.

> **Why this matters.** On a cold boot the OS reads the PRAM to find the boot
> volume and its driver. If the ProFile isn't listed there, `FIND_PM_IDS` fails,
> the OS falls back to "assume Twiggy", computes `bootdev = ProFile(2)+1 = 3 > 2`,
> and bombs with `stup_find_boot` (error 10738, "can't find boot cd"). The
> installer writes the ProFile into PRAM only at clean shutdown; persisting (or
> synthesizing) that PRAM is what lets a standalone cold boot find the ProFile.

## 1. Location and access

The PRAM is **64 bytes = 32 big-endian 16-bit words**, version `4`. It lives in
the floppy/disk controller's shared RAM at logical base **`$FCC181`** and is
battery/standby-backed on real hardware (it survives power-off; the
configuration the installer writes persists). The 68000 reads it with `MOVEP`
(every-other byte on the controller's odd-byte bus): logical word *i* = physical
bytes `$FCC181 + 2·i`. Notable hardware alias: `DVCCODE = $FCC189` (the boot
device code the ROM reads) is physically the high nibble of **byte 4**.

In this emulator the controller RAM is `lisa_fdc_t.ram[]`; the PRAM is
`ram[192..255]` (offset `$180 >> 1 = 192`). It is **volatile** here (zeroed at
init, not battery-backed), so an installed configuration is lost across launches
unless saved. Use the shell methods to persist/restore it:

```
profile.pram_save "<path>"   # write the 64-byte PRAM to a file
profile.pram_load "<path>"   # restore it (call before booting; the ROM reads PRAM at startup)
```

(backed by `lisa_fdc_pram_save` / `lisa_fdc_pram_load` in `src/core/peripherals/lisa_fdc.c`.)

## 2. Field layout (the `pmem` record)

From the `pmem` packed record in `SOURCE-STARTUP.TEXT` (and `TPM` in
`LIBS/LIBPM/LibPM-PMM.TEXT`). `cd_pm_version = 4`.

| Byte(s) | Field | Meaning |
|---|---|---|
| 0–1 | **Version** | must equal `4` (`cd_pm_version`); checked at boot |
| 2–3 | **TimeStamp** | must match the on-disk snapshot's timestamp or the snapshot is rejected |
| 4 (bits 7–4) | **BootVol** | boot-device code — see §4. (Physically the high nibble of `DVCCODE $FCC189`.) |
| 4 (bits 3–0) | NormCont | normal screen contrast 0–15 |
| 5 (7–4 / 3–0) | DimCont / BeepVol | dim contrast 0–15 / beep volume 0–5 |
| 6 | flags + DoubleClick | bit7 MouseOn, bit6 ExtendMem (ROM long-mem-test flag), bit5 ScaleMouse, bit4 pad, bits3–0 DoubleClick |
| 7 (7–4 / 3–0) | FadeDelay / BeginRepeat | screen fade delay / key auto-repeat start |
| 8 (7–4 / 3–0) | SubRepeat / pad | key auto-repeat rate |
| 9 | **CDcount** | number of configured-device entries packed into DevConfig |
| 10–59 | **DevConfig** | up to 50 bytes of variable-length device entries — see §3 |
| 60–61 | MemLoss | memory-loss indicator (0/256/512/768) |
| 62–63 | **Checksum** | 16-bit rotate-sum checksum word — see §5 |

## 3. DevConfig entry encoding (`pack_pm` / `crak_pm`)

The device table (bytes 10–59) is a list of **variable-length** entries packed
back-to-back. The authoritative packer/unpacker is `pack_pm`/`crak_pm`
(`SOURCE-CDCONFIGASM.TEXT`), driven by `GetNxtConfig`/`PutNxtConfig`
(`SOURCE-PMEM.TEXT`). Each entry:

```
byte 0:  bits 7-4 = slot       (configurable-device position; 15 = END-OF-LIST sentinel)
         bits 3-1 = chan       (channel; 7 = emptychan)
         bit  0   = IDsize      (0 = 9-bit driver id [1 id byte]; 1 = 17-bit [2 id bytes])
byte 1:  bits 7-3 = dev        (device; 31 = emptydev)
         bits 2-1 = nExtWords   (count of trailing extension words, 0-3)
         bit  0   = idHi         (most-significant bit of the driver id)
byte 2:  low driver-id byte     → short id = (idHi<<8) | byte2
[byte 3: second id byte]        → long  id = (idHi<<16) | (byte2<<8) | byte3   (only if IDsize=1)
then nExtWords × 2 bytes        extension words (big-endian), driver-specific
```

**Entry length** = `3 + IDsize + 2·nExtWords` bytes. Parsing stops at the first
byte whose slot nibble (bits 7–4) is `15` (`GetNxtConfig` returns `e_nomore`);
`0xFF` filler therefore doubles as the terminator.

**Slot codes** (the slot nibble holds the *internal* configurable-device
position directly — `source-DRIVERDEFS.TEXT`): `cd_slot1=0, cd_slot2=1,
cd_slot3=2, … cd_scc=9, cd_paraport=10, cd_intdisk=11, cd_sony=12, cd_twiggy=13`.
`emptychan = 7`, `emptydev = 31`.

> Packing note: `PutNxtConfig` stores slot/chan/dev as *external−1*, and
> `GetNxtConfig`→`MAKE_INTERNAL` subtracts 1 again, so the **stored slot nibble
> equals the internal `cd_*` constant** (e.g. ProFile = 10). The driver id is
> matched against `SYSTEM.CDD` by `FIND_PM_IDS`/`FIND_CDDS` to locate
> `SYSTEM.CD_<drivername>`; it is **not** hard-coded in the OS — it depends on
> the installed system's `SYSTEM.CDD`.

**Worked example — the ProFile entry `AE F8 23`:**

```
b0 = 0xAE = 1010 1110 → slot = 0xA = 10 (cd_paraport), chan = (0xAE>>1)&7 = 7 (empty), IDsize = 0
b1 = 0xF8 = 1111 1000 → dev  = (0xF8>>3)&0x1F = 31 (empty), nExtWords = 0, idHi = 0
b2 = 0x23            → driver id = (0<<8) | 0x23 = 35
length = 3 + 0 + 0 = 3 bytes
⇒ ProFile (cd_paraport), empty chan/dev, driver id 35
```

## 4. BootVol codes (byte 4, high nibble)

`find_boot` (`SOURCE-STARTUP.TEXT`) dispatches on the boot-device code at
`adr_bootdev=$1b3`, and the `PMCONFIG` tool's `setBootVol`
(`APPS/APPW/apPW-PMCONFIG.text`) documents the same values:

| BootVol | Device |
|---|---|
| 0 | upper floppy (Twiggy 1) |
| 1 | lower floppy (Twiggy 2) / built-in Sony |
| **2** | **parallel-port ProFile** (`cd_paraport`) — on a Lisa 2 (`iob_sony`) |
| 3 / 4 | slot 1 ports 1 / 2 |
| 7 / 8 / 9 | slot 2 ports 1 / 2 / 3 |
| 11 / 12 | slot 3 ports 1 / 2 |

So **`BootVol = 2`** is what makes the ROM/`find_boot` default-boot the ProFile.
(The installer's "Finished → turn off" path writes the ProFile *device entry*
but leaves `BootVol = 0`; "Finished → Start Up from the disk" is what sets
`BootVol = 2`.)

## 5. Checksum (bytes 62–63)

The ROM routine `VFYCHKSM` (= `prom_cksum` at `$FE00BC`, called by `CHKPM` and
by the OS `VERIFY_CKSUM`) is a 16-bit **add-then-rotate** sum over all 32 words:

```asm
        CLR  D3
loop:   MOVE (A0)+,D2        ; next big-endian word (MOVEP form when reading PRAM via the bus)
        ADD  D2,D3           ; 16-bit add
        ROL  #1,D3           ; plain rotate-left 1 (bit15 → bit0; NOT rotate-through-carry)
        DBF  D0,loop         ; D0 = #words-1 = 31 → 32 iterations
        TST  D3              ; VALID iff D3 == 0
```

The stored checksum word (word 31) is the **two's-complement negate** of the
rotate-sum of words 0–30 (`WRTSUM`: rotate-sum 31 words, then `NEG`), chosen so
the full 32-word sum lands on 0.

```python
def rotl16(v):
    v &= 0xFFFF
    return ((v << 1) | (v >> 15)) & 0xFFFF

def pram_checksum_valid(words):            # ROM CHKPM: True iff valid
    acc = 0
    for w in words:                        # all 32 words
        acc = rotl16((acc + (w & 0xFFFF)) & 0xFFFF)
    return acc == 0

def pram_compute_checksum(words31):        # ROM WRTSUM: word 31 for words 0..30
    acc = 0
    for w in words31:                      # 31 words
        acc = rotl16((acc + (w & 0xFFFF)) & 0xFFFF)
    return (-acc) & 0xFFFF
```

Verified: both captured images satisfy `pram_checksum_valid` (full sum `0x0000`),
and `pram_compute_checksum` reproduces their stored words (`0x9798` floppy,
`0x63CA` ProFile).

## 6. Worked decode of the two captured images

**Floppy-only system** (`CDcount=4`, checksum `0x9798`):

```
00 04 0F 20 05 F1 C3 34 10 04 92 F8 20 1E F8 22   Version=4 TS=0F20 BootVol=0 NormCont=5 … CDcount=4
10 F8 23 90 FA 20 C0 20 FF FF FF FF FF FF FF FF
FF … (0xFF filler) … FF 00 4C 97 98              MemLoss=004C Checksum=9798
```

**After installing LOS to the ProFile + clean shutdown** (`CDcount=5`, checksum
`0x63CA`) — verified directly from `lisa.pram`:

| # | bytes | slot | chan | dev | id | meaning |
|---|---|---|---|---|---|---|
| 0 | `92 f8 20` | 9 (cd_scc) | 1 | 31 | 32 | SCC channel |
| 1 | `1e f8 22` | 1 (cd_slot2) | 7 | 31 | 34 | slot device |
| 2 | `10 f8 23` | 1 (cd_slot2) | 0 | 31 | 35 | slot device |
| 3 | `90 fa 20 c0 20` | 9 (cd_scc) | 0 | 31 | 32 | SCC chan (1 ext word `C020`) |
| 4 | **`ae f8 23`** | **10 (cd_paraport)** | 7 | 31 | **35** | **the ProFile** |
| — | `ff…` | 15 | | | | end-of-list |

The only difference the install made: appended entry 4 (the ProFile) and bumped
`CDcount` 4→5 (and the timestamp + checksum). `BootVol` stayed `0` because this
was captured via "turn off" rather than "Start Up".

## 7. Recipe — synthesize a PRAM that boots the ProFile

To make the OS find **and** auto-boot the ProFile you need *both* a `cd_paraport`
device entry (so `FIND_PM_IDS` resolves the driver and avoids the 10738 fallback)
**and** `BootVol = 2` (so the ROM selects it):

1. **Version**: bytes 0–1 = `00 04`.
2. **TimeStamp**: bytes 2–3 — must equal the boot volume's on-disk snapshot
   timestamp, or set both PRAM and snapshot to the same value. (The ROM ignores
   it; only the OS cross-checks PRAM vs. snapshot.)
3. **BootVol**: byte 4 high nibble = `2`; low nibble = NormCont (e.g. `0x26`).
4. **ProFile device entry** in DevConfig (after any existing entries): append
   `AE F8 <id>` where `<id>` is the ProFile driver id from the installed
   `SYSTEM.CDD` (e.g. `0x23`=35 here ⇒ `AE F8 23`), and increment `CDcount`
   (byte 9). Leave the rest of bytes 10–59 as `0xFF` (the `slot=15` terminator).
5. **Checksum**: recompute bytes 62–63 with `pram_compute_checksum(words[0..30])`.

The simplest robust path is to **capture** a real PRAM from a finished install
(`profile.pram_save`) and reuse it, optionally flipping byte 4's high nibble to
`2` and recomputing the checksum. Driver ids are install-specific, so a captured
entry is guaranteed consistent with that system's `SYSTEM.CDD`.

## 8. Sources and notes

- **LisaOS source**: `OS/SOURCE-STARTUP.TEXT`
  (`pmem` record, `find_boot`, `FIND_PM_IDS`, `cd_pm_version`), `OS/SOURCE-PMEM.TEXT`
  (`GetNxtConfig`/`PutNxtConfig`), `OS/SOURCE-CDCONFIGASM.TEXT` (`pack_pm`/`crak_pm`),
  `OS/SOURCE-CD.TEXT` (`MAKE_INTERNAL`, `emptychan`/`emptydev`),
  `OS/source-DRIVERDEFS.TEXT` (`cd_*` slot codes), `OS/source-STARASM2.TEXT`
  (`VERIFY_CKSUM`), `LIBS/LIBPM/LibPM-PMM.TEXT` (`pm_BootVol`↔byte-4 mapping),
  `APPS/APPW/apPW-PMCONFIG.text` (BootVol code table).
- **Boot ROM source** (the "AppleLisa Boot ROM Source", CR line endings):
  `VFYCHKSM`/`WRTSUM`/`CHKPM` (the checksum), `PMWRDS=32`, `PMSTRT=$FCC181`,
  `DVCCODE=$FCC189`, `SAV2PM` (device code → `DVCCODE` upper nibble).
- **Cross-check — other emulators.** Other open-source emulators of this machine
  were surveyed as a sanity check; they treat the whole controller RAM (PRAM
  included) as an opaque byte buffer — no parameter-memory layout struct, no
  checksum routine, no default field values; the raw bytes are persisted verbatim
  and all interpretation is left to the guest ROM/OS. So the authoritative
  reference for this format is the OS/boot-ROM source cited above, not any
  emulator.
- **Caveat.** A correct PRAM controls device *selection/resolution* and is
  required to get past error 10738, but it is upstream of any later OS-init
  issues — necessary, not necessarily sufficient, for a full boot to the desktop.
