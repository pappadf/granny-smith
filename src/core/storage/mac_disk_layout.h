// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mac_disk_layout.h
// On-disk layout of a Macintosh disk, shared by everything that reads or
// builds one: the Driver Descriptor Map in block 0, the Apple Partition Map
// entries from block 1, and the fields of the HFS Master Directory Block and
// HFS Plus Volume Header that identify and size a volume.  The parser
// (image_apm.c), the wrapper's builder and sniff (image_wrap*.c), the HFS
// reader (image_hfs.c) and the volume classifier (image_volume.c) all spell
// them from here, so the copies cannot drift.  Offsets are bytes from the
// start of the block or header named; every field is big-endian.

#ifndef MAC_DISK_LAYOUT_H
#define MAC_DISK_LAYOUT_H

// ---- Driver Descriptor Map (block 0) -----------------------------------------

#define DDM_OFF_SIG        0 // uint16 sbSig 'ER'
#define DDM_OFF_BLK_SIZE   2 // uint16 sbBlkSize
#define DDM_OFF_BLK_COUNT  4 // uint32 sbBlkCount
#define DDM_OFF_DEV_TYPE   8 // uint16 sbDevType
#define DDM_OFF_DEV_ID     10 // uint16 sbDevId
#define DDM_OFF_DRVR_COUNT 16 // uint16 sbDrvrCount
#define DDM_OFF_DD_BLOCK   18 // first descriptor: ddBlock.l, ddSize.w, ddType.w
#define DDM_OFF_DD_SIZE    22
#define DDM_OFF_DD_TYPE    24

#define DDM_SIG_ER 0x4552 // "ER"

// ---- Apple Partition Map entry (one 512-byte block each, from block 1) -------

#define APM_OFF_SIG         0x00 // uint16 pmSig "PM"
#define APM_OFF_SIG_PAD     0x02 // uint16 reserved, should be 0
#define APM_OFF_MAP_BLKCNT  0x04 // uint32 blocks in partition map
#define APM_OFF_PY_START    0x08 // uint32 partition start in 512-byte blocks
#define APM_OFF_PART_BLKCNT 0x0C // uint32 partition size in 512-byte blocks
#define APM_OFF_NAME        0x10 // char[32]
#define APM_OFF_TYPE        0x30 // char[32]
#define APM_OFF_LG_DATA     0x50 // uint32 first logical block of the data area
#define APM_OFF_DATA_CNT    0x54 // uint32 blocks in the data area
#define APM_OFF_STATUS      0x58 // uint32
#define APM_OFF_LG_BOOT     0x5C // uint32 first logical block of boot code
#define APM_OFF_BOOT_SIZE   0x60 // uint32 boot code bytes
#define APM_OFF_BOOT_ADDR   0x64 // uint32 boot code load address
#define APM_OFF_BOOT_ENTRY  0x6C // uint32 boot code entry point
#define APM_OFF_BOOT_CKSUM  0x74 // uint32 boot code checksum
#define APM_OFF_PROCESSOR   0x78 // char[16]
#define APM_PROCESSOR_LEN   16

#define APM_SIG_PM 0x504D // "PM"

// ---- The volume header ------------------------------------------------------

// An HFS MDB, an HFS Plus / HFSX Volume Header and an MFS MDB all sit 1024
// bytes (block 2) into their volume.
#define MAC_VOLUME_HEADER_OFF 1024u

// HFS Master Directory Block fields.
#define MDB_OFF_SIG        0 // uint16 drSigWord "BD"
#define MDB_OFF_NM_AL_BLKS 18 // uint16 drNmAlBlks
#define MDB_OFF_AL_BLK_SIZ 20 // uint32 drAlBlkSiz
#define MDB_OFF_AL_BL_ST   28 // uint16 drAlBlSt, in 512-byte blocks

// HFS Plus Volume Header fields (TN1150).
#define VH_OFF_SIG        0x00 // uint16 "H+" / "HX"
#define VH_OFF_BLOCK_SIZE 0x28 // uint32 allocation block size
#define VH_OFF_TOTAL_BLKS 0x2C // uint32 total allocation blocks

#endif // MAC_DISK_LAYOUT_H
