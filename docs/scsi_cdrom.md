# SCSI CD-ROM Device Emulation

This document specifies the SCSI CD-ROM device emulation for the Granny Smith emulator. It covers the device identity, SCSI command set, mode pages, sense data, block addressing, Apple-specific protocol quirks, and A/UX compatibility requirements. It is the authoritative reference for implementing the CD-ROM device in `scsi_cdrom.c`.

For the NCR 5380 host controller, bus phases, register model, and hard disk command handling, see `docs/scsi.md`.

---

## 1. Device Identity

### 1.1 Emulated Drive

The emulated CD-ROM presents as a **Sony CDU-8002** — the OEM mechanism inside the Apple AppleCD SC Plus (M3021). This is a 1x caddy-loading SCSI-1 drive with a 64 KB buffer and 380 ms average access time.

### 1.2 SCSI INQUIRY Response

The INQUIRY response is 36 bytes minimum (53 bytes with the additional length reported by the real hardware). The device type, removable media bit, SCSI version, and vendor/product strings must be exact — the Apple CD-ROM driver checks them against an internal whitelist.

| Byte(s) | Field | Value | Notes |
|---------|-------|-------|-------|
| 0 | Peripheral Device Type | `0x05` | CD-ROM |
| 1 | RMB / Device-Type Qualifier | `0x80` | Removable media |
| 2 | ISO/ECMA/ANSI Version | `0x01` | SCSI-1 (ANSI X3.131-1986) |
| 3 | Response Data Format | `0x01` | CCS (Common Command Set) |
| 4 | Additional Length | `0x31` | 49 additional bytes (total 53) |
| 5-7 | Reserved/Flags | `0x00` | |
| 8-15 | Vendor Identification | `"    SONY"` | 8 bytes, right-aligned (left-padded with spaces) |
| 16-31 | Product Identification | `"CD-ROM CDU-8002 "` | 16 bytes, space-padded right |
| 32-35 | Product Revision Level | `"1.8g"` | |

**Right-alignment:** Apple drives use right-aligned (left-padded) vendor strings. The existing `scsi_add_device()` function applies this via `sprintf("%8s", vendor)`.

**Whitelist strings recognized by the Apple CD-ROM extension:**

- `CD-ROM CDU-8001` (AppleCD SC)
- `CD-ROM CDU-8002` (AppleCD SC Plus / 150)
- `CD-ROM CDU-8003` (AppleCD 300)
- `CD-ROM CDU-8004` (transitional)
- `CD-ROM CDR-8004` (alternate)
- `CD-ROM CR-8004` (Matsushita, AppleCD 300i/300e Plus)
- `CD-ROM CDU-8005` (AppleCD 600e/600i, Sony)
- `CD-ROM CR-8005` (AppleCD 600e, Matsushita)

### 1.3 Default SCSI ID

CD-ROM drives default to **SCSI ID 3** (matching real Apple hardware convention). Hard disks default to SCSI ID 0. The user can override both via shell commands.

---

## 2. SCSI Command Set

The CDU-8002 implements 36 SCSI commands: 20 mandatory, 6 optional SCSI-2, and 10 Sony vendor-specific. For emulation purposes, the commands are prioritized into tiers based on what Mac OS and A/UX actually issue.

### 2.1 Complete Command Table

| Opcode | Command | CDB Size | Type | Tier |
|--------|---------|----------|------|------|
| `0x00` | TEST UNIT READY | 6 | Mandatory | 1 |
| `0x01` | REZERO UNIT | 6 | Mandatory | 2 |
| `0x03` | REQUEST SENSE | 6 | Mandatory | 1 |
| `0x08` | READ(6) | 6 | Mandatory | 1 |
| `0x0B` | SEEK(6) | 6 | Mandatory | 3 |
| `0x12` | INQUIRY | 6 | Mandatory | 1 |
| `0x15` | MODE SELECT(6) | 6 | Mandatory | 1 |
| `0x16` | RESERVE | 6 | Mandatory | 4 |
| `0x17` | RELEASE | 6 | Mandatory | 4 |
| `0x1A` | MODE SENSE(6) | 6 | Mandatory | 1 |
| `0x1B` | START/STOP UNIT | 6 | Mandatory | 1 |
| `0x1C` | RECEIVE DIAGNOSTIC | 6 | Optional | 4 |
| `0x1D` | SEND DIAGNOSTIC | 6 | Mandatory | 4 |
| `0x1E` | PREVENT/ALLOW MEDIUM REMOVAL | 6 | Mandatory | 1 |
| `0x25` | READ CAPACITY | 10 | Mandatory | 1 |
| `0x28` | READ(10) | 10 | Mandatory | 1 |
| `0x2B` | SEEK(10) | 10 | Mandatory | 3 |
| `0x2F` | VERIFY | 10 | Optional | 3 |
| `0x3B` | WRITE BUFFER | 10 | Optional | 4 |
| `0x3C` | READ BUFFER | 10 | Optional | 4 |
| `0x42` | READ SUB-CHANNEL | 10 | SCSI-2 | 1 |
| `0x43` | READ TOC | 10 | SCSI-2 | 1 |
| `0x44` | READ HEADER | 10 | SCSI-2 | 2 |
| `0x45` | PLAY AUDIO(10) | 10 | SCSI-2 | 3 |
| `0x47` | PLAY AUDIO MSF | 10 | SCSI-2 | 3 |
| `0x48` | PLAY AUDIO TRACK/INDEX | 10 | SCSI-2 | 3 |
| `0x4B` | PAUSE/RESUME | 10 | SCSI-2 | 3 |
| `0xC1` | READ TOC (Sony) | 10 | Vendor | 3 |
| `0xC2` | READ SUB-CHANNEL (Sony) | 10 | Vendor | 3 |
| `0xC3` | READ HEADER (Sony) | 10 | Vendor | 3 |
| `0xC4` | PLAYBACK STATUS (Sony) | 10 | Vendor | 3 |
| `0xC5` | PAUSE (Sony) | 10 | Vendor | 3 |
| `0xC6` | PLAY TRACK (Sony) | 10 | Vendor | 3 |
| `0xC7` | PLAY MSF (Sony) | 10 | Vendor | 3 |
| `0xC8` | PLAY AUDIO (Sony) | 10 | Vendor | 3 |
| `0xC9` | PLAYBACK CONTROL (Sony) | 10 | Vendor | 3 |

**Tier definitions:**
- **Tier 1:** Required for Mac OS to recognize and mount a data CD.
- **Tier 2:** Required for A/UX or for specific driver versions.
- **Tier 3:** Audio playback and seek commands. Stub with ILLEGAL REQUEST for data-only discs.
- **Tier 4:** Diagnostic/reservation commands. Minimal stubs or no-ops.

### 2.2 Sony Vendor Commands (0xC1-0xC9)

The CDU-8002 implements Sony proprietary audio commands alongside their SCSI-2 equivalents. Early Apple CD-ROM drivers (System 6, early System 7) and the Apple CD Remote DA use **only** the Sony vendor commands for audio playback. Later drivers (AppleCD 300 series onward) switched to SCSI-2 standard commands.

For data-only disc emulation (no audio CD playback), these commands should return CHECK CONDITION with ILLEGAL REQUEST sense key and ASC `0x64` "ILLEGAL MODE FOR THIS TRACK" or `0x30` "INCOMPATIBLE MEDIUM INSTALLED".

---

## 3. Command Specifications

### 3.1 TEST UNIT READY — `0x00` (6 bytes)

**CDB:** `00 (lun<<5) 00 00 00 00`

**Behavior:**
- UNIT ATTENTION pending → CHECK CONDITION (see section 6)
- No media → CHECK CONDITION, sense key NOT READY (`0x02`), ASC `0x3A` MEDIUM NOT PRESENT
- Media present and ready → STATUS GOOD

### 3.2 REQUEST SENSE — `0x03` (6 bytes)

**CDB:** `03 (lun<<5) 00 00 <alloc_len> 00`

**Response:** Fixed-format sense data (18 bytes minimum):

| Byte | Field | Value |
|------|-------|-------|
| 0 | Response Code | `0x70` (current errors) |
| 1 | Obsolete | `0x00` |
| 2 | Sense Key | 4-bit key (see section 6) |
| 3-6 | Information | Context-dependent (e.g., LBA of error) |
| 7 | Additional Sense Length | `0x0A` (10 additional bytes) |
| 8-11 | Command-specific | `0x00` |
| 12 | ASC | Additional Sense Code |
| 13 | ASCQ | Additional Sense Code Qualifier |
| 14-17 | Reserved | `0x00` |

**Behavior:**
- Always returns STATUS GOOD (never CHECK CONDITION)
- Returns and clears the pending sense data for the requesting initiator
- If UNIT ATTENTION is pending, returns it and clears the condition
- If no error is pending, returns sense key NO SENSE (`0x00`)
- Does **not** clear UNIT ATTENTION condition (only reports it; the condition is cleared after the report)

### 3.3 INQUIRY — `0x12` (6 bytes)

**CDB:** `12 (lun<<5|evpd) <page_code> 00 <alloc_len> 00`

**Behavior:**
- EVPD=0: Return standard INQUIRY data per section 1.2
- EVPD=1: Not supported — return CHECK CONDITION with ILLEGAL REQUEST
- Does **not** clear UNIT ATTENTION (per SCSI spec: INQUIRY is exempt)
- Transfer length capped to allocation length

### 3.4 MODE SENSE(6) — `0x1A` (6 bytes)

**CDB:** `1A (dbd<<3|lun<<5) <pc_page> 00 <alloc_len> 00`

Where `pc_page` byte encodes: bits 7-6 = Page Control (PC), bits 5-0 = Page Code.

**Response structure:**

```
[Mode Parameter Header — 4 bytes]
  Byte 0: Mode Data Length (total response length minus 1)
  Byte 1: Medium Type (0x00)
  Byte 2: Device-Specific Parameter (0x00)
  Byte 3: Block Descriptor Length (0x08)

[Block Descriptor — 8 bytes, ALWAYS PRESENT]
  Bytes 0:    Density Code (0x00)
  Bytes 1-3:  Number of Blocks (image_size / block_size, 24-bit big-endian)
  Byte 4:     Reserved (0x00)
  Bytes 5-7:  Block Length (current block_size, 24-bit big-endian)

[Mode Page(s) — variable]
```

**The block descriptor must always be present**, regardless of the DBD bit in the CDB. A/UX does not set DBD and expects the descriptor. Omitting it causes A/UX to fail during SCSI bus enumeration.

**Page Control (PC) values:**
- `0x00` — Current values
- `0x01` — Changeable values (return bitmask of modifiable fields)
- `0x02` — Default values
- `0x03` — Saved values (not supported — return CHECK CONDITION, ILLEGAL REQUEST)

**Supported mode pages:** See section 4.

### 3.5 MODE SELECT(6) — `0x15` (6 bytes)

**CDB:** `15 (pf<<4|sp<<0|lun<<5) 00 00 <param_list_len> 00`

**Behavior:**
- Accept the parameter list header (4 bytes) and optional block descriptor (8 bytes)
- Parse block descriptor to detect **block size change** (see section 5.2)
- Parse mode pages if present
- **Accept PF=0** (see section 5.3)
- **Accept truncated parameter lists** (see section 5.5)
- SP (Save Pages) bit is ignored — the drive cannot save parameters

### 3.6 READ(6) — `0x08` (6 bytes)

**CDB layout:**

| Byte | Bits | Field |
|------|------|-------|
| 0 | 7-0 | Opcode `0x08` |
| 1 | 7-5 | LUN |
| 1 | 4-0 | LBA bits 20-16 |
| 2 | 7-0 | LBA bits 15-8 |
| 3 | 7-0 | LBA bits 7-0 |
| 4 | 7-0 | Transfer Length (0 = 256 blocks) |
| 5 | 7-0 | Control |

**Byte offset calculation:**
```c
byte_offset = lba * device->block_size;
byte_count  = transfer_length * device->block_size;
```

The `block_size` is the SCSI logical block size (default 2048, switchable to 512). The storage layer always reads in 512-byte physical blocks; this works because both 2048 and 512 are multiples of 512.

**Error conditions:**
- LBA + transfer_length exceeds capacity → CHECK CONDITION, ILLEGAL REQUEST, ASC `0x21` LBA OUT OF RANGE
- LBA falls in a non-data track → CHECK CONDITION, ILLEGAL REQUEST, ASC `0x64` ILLEGAL MODE FOR THIS TRACK
- Write command to CD-ROM → CHECK CONDITION, DATA PROTECT (`0x07`), ASC `0x27` WRITE PROTECTED

### 3.7 READ(10) — `0x28` (10 bytes)

**CDB layout:**

| Byte | Field |
|------|-------|
| 0 | Opcode `0x28` |
| 1 | Flags (LUN obsolete) |
| 2-5 | LBA (32-bit big-endian) |
| 6 | Reserved |
| 7-8 | Transfer Length (16-bit big-endian) |
| 9 | Control |

Transfer length of 0 means no data transfer (seek only). The Apple CD-ROM driver uses READ(10) rather than READ(6) for CD-ROM access.

### 3.8 READ CAPACITY — `0x25` (10 bytes)

**CDB:** `25 00 00 00 00 00 00 00 00 00`

**Response (8 bytes):**

| Bytes | Field | Value |
|-------|-------|-------|
| 0-3 | Last LBA | `(image_size / block_size) - 1` (big-endian) |
| 4-7 | Block Length | `block_size` (big-endian) |

The block size reflects the current logical block size (2048 default, or 512 after MODE SELECT switch). The capacity in blocks changes accordingly — a 650 MB image has 317,382 blocks at 2048 bytes or 1,331,200 blocks at 512 bytes.

### 3.9 START/STOP UNIT — `0x1B` (6 bytes)

**CDB:** `1B 00 00 00 <start_loej> 00`

Where byte 4: bit 1 = LoEj (Load/Eject), bit 0 = Start.

| Start | LoEj | Action |
|-------|------|--------|
| 1 | 0 | Spin up (no-op — always ready) |
| 0 | 0 | Spin down (no-op) |
| 0 | 1 | Eject disc |
| 1 | 1 | Load disc (no-op for emulator) |

**Eject:** If medium removal is prevented (via PREVENT/ALLOW MEDIUM REMOVAL), return CHECK CONDITION with ILLEGAL REQUEST. Otherwise, detach the image and set UNIT ATTENTION with ASC `0x3A` MEDIUM NOT PRESENT for subsequent commands.

**Note:** The physical eject button on real AppleCD SC Plus hardware only works with Apple II computers. On Macintosh, ejection is exclusively software-controlled.

### 3.10 PREVENT/ALLOW MEDIUM REMOVAL — `0x1E` (6 bytes)

**CDB:** `1E 00 00 00 <prevent> 00`

Where byte 4 bit 0: 1 = prevent removal, 0 = allow removal.

Store the prevent state per-device. When prevent=1, eject operations (START/STOP UNIT with LoEj=1) are refused with ILLEGAL REQUEST. The prevention terminates on ALLOW MEDIUM REMOVAL, BUS DEVICE RESET, or bus reset.

### 3.11 READ TOC — `0x43` (10 bytes)

**CDB layout:**

| Byte | Field |
|------|-------|
| 0 | Opcode `0x43` |
| 1 | MSF bit (bit 1): 0 = LBA format, 1 = MSF format |
| 2-5 | Reserved |
| 6 | Starting Track/Session Number |
| 7-8 | Allocation Length (big-endian) |
| 9 | Format (bits 7-6 of byte 2 in some implementations, or byte 9 bits 5-0) |

**Format 0 — TOC (default):**

Response for a single-track data disc:

```
[TOC Header — 4 bytes]
  Bytes 0-1: TOC Data Length (big-endian, excludes these 2 bytes)
  Byte 2:    First Track Number (0x01)
  Byte 3:    Last Track Number (0x01)

[Track Descriptor — Track 1, 8 bytes]
  Byte 0:    Reserved (0x00)
  Byte 1:    ADR (bits 7-4) | Control (bits 3-0) = 0x14 (data track, digital copy prohibited)
  Byte 2:    Track Number (0x01)
  Byte 3:    Reserved (0x00)
  Bytes 4-7: Track Start Address (LBA 0 or MSF 00:02:00)

[Lead-out Descriptor — Track 0xAA, 8 bytes]
  Byte 0:    Reserved (0x00)
  Byte 1:    ADR/Control = 0x14
  Byte 2:    Track Number (0xAA = lead-out)
  Byte 3:    Reserved (0x00)
  Bytes 4-7: Lead-out Start Address (= total user blocks)
```

**Control field bits for track descriptors:**

| Bit | =1 | =0 |
|-----|----|----|
| 0 | Pre-emphasis | No pre-emphasis |
| 1 | Digital copy permitted | Copy prohibited |
| 2 | Data track | Audio track |
| 3 | Four-channel | Two-channel |

For data tracks: Control = `0x04` (data, no pre-emphasis, two-channel, copy prohibited). ADR = `0x01` (Q sub-channel encodes current position). Combined: `0x14`.

**Format 1 — Session Info:**

Returns the first and last session numbers and the start address of the last session's first track. For a single-session disc, this is identical to the TOC header with the track 1 start address.

**MSF conversion:** When MSF bit = 1, LBA addresses are converted to Minutes:Seconds:Frames format. The conversion from LBA to MSF (with the standard 2-second / 150-frame offset for lead-in):

```
lba += 150;
frames  = lba % 75;
seconds = (lba / 75) % 60;
minutes = lba / (75 * 60);
```

### 3.12 READ SUB-CHANNEL — `0x42` (10 bytes)

**CDB layout:**

| Byte | Field |
|------|-------|
| 0 | Opcode `0x42` |
| 1 | MSF bit (bit 1) |
| 2 | SubQ bit (bit 6): 1 = return sub-channel data, 0 = header only |
| 3 | Data Format: `0x01` = CD current position, `0x02` = Media catalog number, `0x03` = ISRC |
| 4-5 | Reserved |
| 6 | Track Number |
| 7-8 | Allocation Length |
| 9 | Control |

**Response for Data Format 0x01 (Current Position), data-only disc:**

| Byte | Field | Value |
|------|-------|-------|
| 0 | Reserved | `0x00` |
| 1 | Audio Status | `0x15` (no current audio status) |
| 2-3 | Sub-channel Data Length | `0x000C` (12 bytes) if SubQ=1 |
| 4 | Data Format Code | `0x01` |
| 5 | ADR/Control | `0x14` (data track) |
| 6 | Track Number | `0x01` |
| 7 | Index Number | `0x01` |
| 8-11 | Absolute CD Address | `0x00000000` |
| 12-15 | Track Relative Address | `0x00000000` |

Mac OS polls READ SUB-CHANNEL periodically to check audio playback status. For a data-only disc, return "no current audio status" (`0x15`) consistently.

### 3.13 READ HEADER — `0x44` (10 bytes)

**CDB layout:**

| Byte | Field |
|------|-------|
| 0 | Opcode `0x44` |
| 1 | MSF bit (bit 1) |
| 2-5 | LBA (big-endian) |
| 6-8 | Reserved |
| 7-8 | Allocation Length |
| 9 | Control |

**Response (8 bytes):**

| Byte | Field | Value |
|------|-------|-------|
| 0 | CD-ROM Data Mode | `0x01` (Mode 1) |
| 1-3 | Reserved | `0x00` |
| 4-7 | Absolute CD-ROM Address | Requested LBA (or MSF if MSF bit set) |

For a data-only ISO 9660 image, all blocks are Mode 1. The mode field indicates whether the sector contains Mode 1 data (2048 user bytes + ECC) or Mode 2 data (2336 user bytes, no ECC).

### 3.14 REZERO UNIT — `0x01` (6 bytes)

Seek to LBA 0. Return STATUS GOOD.

### 3.15 SEEK(6) — `0x0B` / SEEK(10) — `0x2B`

Seek to the specified LBA. Return STATUS GOOD. No actual seek latency is emulated.

---

## 4. Mode Pages

The CDU-8002 supports five standard mode pages plus the Apple vendor page. All pages follow the standard format: `[page_code] [page_length] [page_data...]`.

### 4.1 Page 0x01 — Read Error Recovery Parameters (6 bytes)

| Byte | Field | Default |
|------|-------|---------|
| 0 | Page Code | `0x01` |
| 1 | Page Length | `0x06` |
| 2 | Error Recovery Parameter | `0x00` (max recovery, report L-EC uncorrectable only) |
| 3 | Read Retry Count | `0x03` |
| 4-7 | Reserved | `0x00` |

The error recovery parameter byte encodes a combination of TB, RC, PER, DTE, and DCR bits per the Sony CDU-541 manual. For emulation, the default (`0x00`) means maximum error recovery with only uncorrectable errors reported.

**Changeable mask:** Byte 2 bits (error recovery parameter) and byte 3 (retry count) are changeable.

### 4.2 Page 0x02 — Disconnect-Reconnect Parameters (14 bytes)

| Byte | Field | Default |
|------|-------|---------|
| 0 | Page Code | `0x02` |
| 1 | Page Length | `0x0E` |
| 2 | Buffer Full Ratio | `0x00` |
| 3 | Buffer Empty Ratio | `0x00` |
| 4-5 | Bus Inactivity Limit | `0x0000` |
| 6-7 | Disconnect Time Limit | `0x0000` |
| 8-9 | Connect Time Limit | `0x0000` |
| 10-11 | Maximum Burst Size | `0x0000` |
| 12 | DTDC | `0x00` |
| 13-15 | Reserved | `0x00` |

Return all zeros — the emulator does not disconnect/reconnect.

**Changeable mask:** All fields changeable (accept MODE SELECT but ignore values).

### 4.3 Page 0x07 — Verify Error Recovery Parameters (6 bytes)

| Byte | Field | Default |
|------|-------|---------|
| 0 | Page Code | `0x07` |
| 1 | Page Length | `0x06` |
| 2 | Error Recovery Parameter | `0x00` |
| 3 | Verify Retry Count | `0x03` |
| 4-7 | Reserved | `0x00` |

Same structure as page 0x01 but for VERIFY command error handling.

### 4.4 Page 0x08 — CD-ROM Parameters (6 bytes)

| Byte | Field | Default |
|------|-------|---------|
| 0 | Page Code | `0x08` |
| 1 | Page Length | `0x06` |
| 2 | Reserved | `0x00` |
| 3 | Inactivity Timer Multiplier | `0x00` (vendor-specific default) |
| 4-5 | S-Units per M-Unit / F-Units per S-Unit | `0x0000` |
| 6 | Reserved | `0x00` |
| 7 | LBAMSF bit (bit 0) | `0x00` (LBA format) |

The LBAMSF bit controls the default address format for audio playback status reporting. When 0, addresses are in LBA format; when 1, in MSF format.

**Changeable mask:** Byte 3 (inactivity timer) and byte 7 bit 0 (LBAMSF) are changeable.

### 4.5 Page 0x09 — CD-ROM Audio Control Parameters (14 bytes)

| Byte | Field | Default |
|------|-------|---------|
| 0 | Page Code | `0x09` |
| 1 | Page Length | `0x0E` |
| 2 | Immd (bit 2), SOTC (bit 1) | `0x00` |
| 3-5 | Reserved | `0x00` |
| 6 | Channel 0 Output Selection | `0x01` (output to port 0) |
| 7 | Channel 0 Volume | `0xFF` (max) |
| 8 | Channel 1 Output Selection | `0x02` (output to port 1) |
| 9 | Channel 1 Volume | `0xFF` (max) |
| 10 | Channel 2 Output Selection | `0x00` (muted) |
| 11 | Channel 2 Volume | `0x00` |
| 12 | Channel 3 Output Selection | `0x00` (muted) |
| 13 | Channel 3 Volume | `0x00` |
| 14-15 | Reserved | `0x00` |

Channel output selection nibble values: `0x00` = muted, `0x01` = port 0, `0x02` = port 1, `0x04` = port 2 (not supported), `0x08` = port 3 (not supported).

The Immd bit controls whether audio play commands return status immediately (Immd=1) or wait for completion (Immd=0). SOTC (Stop On Track Crossing) controls whether audio playback stops when crossing a track boundary.

**Changeable mask:** Immd, SOTC, all channel selection and volume fields.

### 4.6 Page 0x30 — Apple Vendor Identification (32 bytes)

| Byte | Field | Value |
|------|-------|-------|
| 0 | Page Code | `0x30` |
| 1 | Page Length | `0x1E` (30 bytes) |
| 2-24 | Identification String | `"APPLE COMPUTER, INC   "` (23 bytes ASCII) |
| 25-31 | Zero Padding | `0x00` x 7 |

ASCII hex of the identification string: `41 50 50 4C 45 20 43 4F 4D 50 55 54 45 52 2C 20 49 4E 43 20 20 20`.

**Page Control behavior:**
- PC=0 (current): Return the string as above
- PC=1 (changeable): Return all zeros (nothing is changeable)
- PC=2 (default): Return the string as above

This page is the **primary authentication gate** for the Apple CD-ROM driver. Beyond the INQUIRY whitelist, the driver sends MODE SENSE for page 0x30 and verifies the Apple identification string. Drives that fail this check are rejected by the driver.

**Historical note:** The real CDU-8002 (SCSI-1 era, 1991) may predate this mechanism — the Sony CDU-541 manual does not document page 0x30, and early Apple drivers relied solely on INQUIRY product strings. However, later Apple drivers (System 7.5+) request page 0x30 from all drives, and we implement it for broad compatibility. This rationale should be noted in a code comment.

### 4.7 Page 0x3F — Return All Pages

When page code `0x3F` is requested, concatenate all supported pages (0x01, 0x02, 0x07, 0x08, 0x09, 0x30) in ascending order after the mode parameter header and block descriptor.

---

## 5. Apple-Specific Protocol Quirks

These five behaviors deviate from strict SCSI-2 compliance. They are required for compatibility with Apple CD-ROM drivers and A/UX. All five are validated against real hardware behavior.

### 5.1 MODE PAGE 0x30 — Apple Vendor Identification

See section 4.6. The driver requests this page during device enumeration and rejects drives that do not return the `"APPLE COMPUTER, INC   "` string.

**Exception:** Apple CD-ROM extension version 5.3.1 (bundled with Mac OS 7.6) removes all vendor checks entirely and works with any SCSI CD-ROM. Version 5.4.2 (Mac OS 8.1) re-enabled them.

### 5.2 Block Size Switching via MODE SELECT

A/UX sends MODE SELECT to change the CD-ROM logical block size from 2048 to 512 bytes. The A/UX installation CD uses an HFS filesystem with 512-byte logical blocks, and A/UX's SCSI subsystem addresses all sectors in 512-byte units.

**Implementation:** Parse the block descriptor in the MODE SELECT parameter list. If the Block Length field (bytes 5-7) specifies 512, set `devices[target].block_size = 512`. All subsequent READ, READ CAPACITY, and MODE SENSE commands use this block size. If the field specifies 2048, restore the default.

**Without this, A/UX fails with `panic: no root file system` during installation.**

This is the same requirement as contemporaneous Sun workstations and is the primary cause of A/UX CD-ROM incompatibility with non-Apple drives.

### 5.3 PF=0 MODE SELECT for Page 0x00

A/UX sends MODE SELECT with the Page Format bit cleared (byte 1 bit 4 = 0) during SCSI bus enumeration. Per SCSI-2, PF=1 is required for page-formatted parameters, but A/UX sends PF=0 with vendor-specific page 0x00 content.

**Implementation:** Do not reject MODE SELECT when PF=0. Accept the data and parse the block descriptor normally. Ignore any page content (or treat it as vendor-specific page 0x00).

**Without this, A/UX enters an infinite loop during SCSI bus enumeration.**

### 5.4 Block Descriptor Always Present in MODE SENSE

A/UX does not set the DBD (Disable Block Descriptors) bit in MODE SENSE requests and expects the 8-byte block descriptor to be present in the response. The SCSI-2 spec allows targets to omit the block descriptor when DBD=1, but A/UX never sets it.

**Implementation:** Always include the block descriptor in MODE SENSE responses for CD-ROM devices, regardless of the DBD bit value.

### 5.5 Truncated MODE SELECT Accepted

Both Mac OS and A/UX send MODE SELECT commands where the parameter list is shorter than the full page length. For example, a MODE SELECT intended for page 0x00 may send only the 4-byte header and 8-byte block descriptor without any page data, or a page may be cut short.

**Implementation:** Accept the parameter list as-is. Do not validate that the data length matches the expected page length. Process whatever bytes are provided and ignore missing trailing bytes. Do not return CHECK CONDITION for truncated pages.

---

## 6. Sense Data and Error Reporting

### 6.1 Per-Device Sense State

Each SCSI device maintains a sense data buffer:

```c
struct {
    uint8_t key;    // sense key (4-bit)
    uint8_t asc;    // additional sense code
    uint8_t ascq;   // additional sense code qualifier
} sense;
```

The sense buffer is populated when a command terminates with CHECK CONDITION status. It is returned by the next REQUEST SENSE command and then cleared.

### 6.2 Sense Keys

| Code | Name | Usage |
|------|------|-------|
| `0x00` | NO SENSE | No error (returned when no pending sense data) |
| `0x02` | NOT READY | No disc, drive not ready, TOC read in progress |
| `0x03` | MEDIUM ERROR | Unrecoverable read error |
| `0x04` | HARDWARE ERROR | Mechanical failure |
| `0x05` | ILLEGAL REQUEST | Invalid command, parameter, or field |
| `0x06` | UNIT ATTENTION | Media change, power-on, or MODE SELECT from another initiator |
| `0x07` | DATA PROTECT | Write attempt to read-only device |

### 6.3 Additional Sense Codes (ASC/ASCQ)

| ASC | ASCQ | Meaning | Context |
|-----|------|---------|---------|
| `0x20` | `0x00` | Invalid Command Operation Code | Unsupported opcode |
| `0x21` | `0x00` | Logical Block Address Out of Range | LBA exceeds capacity |
| `0x24` | `0x00` | Invalid Field in CDB | Bad parameter in command |
| `0x26` | `0x00` | Invalid Field in Parameter List | Bad MODE SELECT data |
| `0x27` | `0x00` | Write Protected | Write to CD-ROM |
| `0x28` | `0x00` | Not Ready to Ready Change | Media inserted (UNIT ATTENTION) |
| `0x29` | `0x00` | Power On, Reset, or Bus Device Reset | Power-on/reset (UNIT ATTENTION) |
| `0x2A` | `0x00` | Parameters Changed | MODE SELECT from another initiator |
| `0x30` | `0x00` | Incompatible Medium Installed | Wrong disc type |
| `0x3A` | `0x00` | Medium Not Present | No disc in drive |
| `0x53` | `0x02` | Medium Removal Prevented | Eject while locked |
| `0x64` | `0x00` | Illegal Mode for This Track | Data command on audio track (or vice versa) |

### 6.4 UNIT ATTENTION

UNIT ATTENTION is a per-initiator condition. It is set on:

1. **Power-on / reset:** ASC/ASCQ `0x29/0x00`
2. **Media change (insert):** ASC/ASCQ `0x28/0x00`
3. **Media removal:** ASC/ASCQ `0x3A/0x00`
4. **MODE SELECT from another initiator:** ASC/ASCQ `0x2A/0x00`

**Clearing behavior:**
- The condition persists until the initiator sends a command that receives CHECK CONDITION
- If the next command from that initiator is REQUEST SENSE, the UNIT ATTENTION sense key is returned and the condition is cleared
- If any other command is received, CHECK CONDITION is returned and the condition is cleared
- **INQUIRY does not clear UNIT ATTENTION** — it executes normally with the condition still pending
- **START/STOP UNIT with LoEj=1** does not clear UNIT ATTENTION — it executes normally

**Priority (when multiple conditions pending):**
1. Power on / reset (highest)
2. Not ready to ready transition (medium change)
3. MODE SELECT from another initiator (lowest)

---

## 7. Block Addressing and Disc Geometry

### 7.1 Logical Block Addressing

All disc addressing uses LBA (Logical Block Address). The first user-accessible block is LBA 0. The total number of blocks is reported by READ CAPACITY.

For a raw ISO 9660 image:
- Total blocks = `image_size / block_size`
- Last LBA = total blocks - 1

### 7.2 SCSI Logical Block Size vs. Storage Physical Block Size

The per-device `block_size` field is a **SCSI logical block size**. It defaults to 2048 for CD-ROM and is switchable to 512 via MODE SELECT.

The storage layer always operates in 512-byte physical blocks (`STORAGE_BLOCK_SIZE`). This does not change. A single 2048-byte logical read translates to four 512-byte physical reads internally. The caller computes byte offsets and byte counts as multiples of the logical block size; the storage layer requires only that these be multiples of 512 (which they always are, since 2048 = 4 x 512).

### 7.3 MSF Addressing

Some commands support MSF (Minutes:Seconds:Frames) addressing as an alternative to LBA. The MSF format encodes absolute disc position with 75 frames per second:

**LBA to MSF (with 150-frame / 2-second lead-in offset):**
```
adjusted = lba + 150;
M = adjusted / (60 * 75);
S = (adjusted / 75) % 60;
F = adjusted % 75;
```

**MSF to LBA:**
```
lba = (M * 60 * 75) + (S * 75) + F - 150;
```

The first user data on a disc starts at MSF 00:02:00 (LBA 0).

### 7.4 Supported Block Lengths

| Block Length | Description |
|-------------|-------------|
| 2048 bytes | CD-ROM Mode 1 user data (default) |
| 2336 bytes | CD-ROM Mode 2 user data |
| 2340 bytes | CD-ROM Mode 2 with sub-header |
| 2352 bytes | Raw block data (all 98 frames, used for audio) |
| 512 bytes | A/UX compatibility mode (via MODE SELECT) |

The emulator implements 2048 (default) and 512 (A/UX mode). Mode 2 and raw block lengths are not implemented but could be added for audio CD or Mode 2 disc support.

### 7.5 Table of Contents Generation

Raw ISO images do not contain TOC metadata. The emulator synthesizes a minimal single-session, single-track data TOC:

- First track: 1
- Last track: 1
- Track 1: data track (Control = 0x04), starts at LBA 0
- Lead-out: starts at LBA = `image_size / block_size`

If bin/cue format support is added later, the TOC would be parsed from the cue sheet instead of synthesized.

---

## 8. CD-ROM Image Formats

### 8.1 Raw ISO 9660

The primary supported format. A raw sector dump with 2048 bytes per sector. File extension: `.iso`. The image size must be a multiple of 512 bytes (enforced by `image_open()`; in practice all ISO images are 2048-aligned which satisfies this).

### 8.2 Raw HFS

A raw HFS volume image. Identified by the Master Directory Block (MDB) signature `0x4244` at byte offset 1024. May also be a hybrid ISO 9660 + HFS disc (both signatures present).

### 8.3 Image Validation

The `cdrom validate` shell command checks for:

1. **ISO 9660 signature:** Bytes at offset 32769 (sector 16, byte 1) = `"CD001"` (5 bytes). This is the Primary Volume Descriptor identifier defined by ISO 9660.

2. **HFS signature:** Big-endian uint16 at byte offset 1024 = `0x4244`. This is the drSigWord field of the HFS Master Directory Block.

3. **Floppy rejection:** Images matching floppy sizes (400K, 800K, 1440K) are rejected — use `fd validate` instead.

4. **Size range:** Typical CD-ROM images are 100 MB to 700 MB. Images outside this range produce a warning but are not rejected.

### 8.4 Read-Only Enforcement

CD-ROM images are always opened read-only (`writable = false` in `image_open()`). The storage engine creates delta and journal file paths but never writes to them. SCSI WRITE commands to the CD-ROM device return CHECK CONDITION with DATA PROTECT sense key.

---

## 9. Software Compatibility

### 9.1 Apple CD-ROM Driver Versions

| Version | Era | Vendor Check | Notes |
|---------|-----|-------------|-------|
| AppleCD SC Setup 2.0.1-3.2 | System 6 | INQUIRY whitelist only | Supports CDU-8001, CDU-8002 |
| CD-ROM Setup 4.0-4.0.5 | System 7.0+ | INQUIRY whitelist | Added AppleCD 300 support |
| Apple CD-ROM 5.0-5.0.4 | System 7.1+ | INQUIRY whitelist + page 0x30 | Added 4x support |
| Apple CD-ROM 5.3.1 | Mac OS 7.6 | **No vendor check** | Universal driver, works with any SCSI CD-ROM |
| Apple CD-ROM 5.4.2 | Mac OS 8.1 | INQUIRY whitelist + page 0x30 | Re-enabled vendor checks |
| Apple CD/DVD Driver 1.0.1+ | Mac OS 8.5+ | INQUIRY whitelist + page 0x30 | ATAPI + SCSI |

The CDU-8002 identity passes all whitelist checks across all driver versions.

### 9.2 Required Extension Suite

A complete Mac OS CD-ROM installation requires multiple cooperating extensions:

- **Apple CD-ROM** (or equivalent version) — SCSI driver
- **Foreign File Access** — framework for non-HFS filesystem plugins
- **ISO 9660 File Access** — ISO 9660/High Sierra filesystem support
- **High Sierra File Access** — older CD-ROM filesystem standard
- **Audio CD Access** — audio disc recognition and playback

Without Foreign File Access, only HFS-formatted CDs are readable.

### 9.3 A/UX Compatibility

A/UX (Apple's UNIX for 68K Macs) bypasses the Macintosh Toolbox and uses kernel-level SCSI drivers. CD-ROMs appear as block devices at `/dev/dsk/cXdYsZ`.

**Requirements beyond standard Mac OS:**
- 512-byte sector mode switching (section 5.2) — **critical**
- PF=0 MODE SELECT (section 5.3)
- Block descriptor always present (section 5.4)
- Truncated MODE SELECT (section 5.5)

A/UX versions 2.0 through 3.1.1 support CD-ROM. A/UX 3.0+ boots into System 7.0.1 for installation and includes a modified Apple CD-ROM driver on the boot floppy. The CD-ROM is typically at SCSI ID 3.

### 9.4 CD Boot Support

The CDU-8002 supports CD boot on 68030/68040 Macs (Quadra series, LC III, etc.). The following Macs **cannot** boot from CD: Mac Plus, SE, Classic, Mac II, IIx, IIcx, SE/30, Portable.

Requirements for a bootable CD: HFS filesystem, Apple boot driver on disc, compatible System Folder, single-session closed disc. Boot is triggered by holding the C key at startup.

---

## 10. Shell Commands

### 10.1 `cdrom validate <path>`

Validate a CD-ROM image file. Returns `cmd_bool(true)` if the image is valid, `cmd_bool(false)` otherwise. Prints diagnostic information (size, detected filesystem) to the terminal.

### 10.2 `cdrom attach <path> [id]`

Attach a CD-ROM image to the SCSI bus. Default SCSI ID is 3. The image is opened read-only and the device presents as a Sony CDU-8002 with all Apple-compatible INQUIRY and MODE page responses.

### 10.3 `cdrom eject [id]`

Eject the CD-ROM at the specified SCSI ID (default 3). Sets UNIT ATTENTION with ASC `0x3A` MEDIUM NOT PRESENT. Fails if medium removal is prevented.

### 10.4 `cdrom info [id]`

Display information about the attached CD-ROM: image path, filesystem type, image size, current block size, and SCSI ID.

### 10.5 Comparison with `hd` Command

| Aspect | `hd` | `cdrom` |
|--------|------|---------|
| Attach | `hd attach <path> [id]` | `cdrom attach <path> [id]` |
| Default ID | 0 | 3 |
| Validate | `hd validate <path>` | `cdrom validate <path>` |
| Read-only | No | Yes |
| Eject | N/A | `cdrom eject [id]` |
| Info | N/A | `cdrom info [id]` |

---

## 11. Source Files

| File | Contents |
|------|----------|
| `src/core/peripherals/scsi.h` | Public SCSI API |
| `src/core/peripherals/scsi_internal.h` | Shared types, constants, struct definitions |
| `src/core/peripherals/scsi.c` | NCR 5380 controller, command dispatch, HD handling |
| `src/core/peripherals/scsi_cdrom.c` | CD-ROM device: INQUIRY, MODE pages, READ TOC, sense, Apple quirks |
| `src/core/system.c` | Shell commands (`cdrom validate/attach/eject/info`) |
| `src/core/storage/image.h` | Image types including `image_cdrom` |
