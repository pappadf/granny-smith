// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// object_order.h
// The display-order weights (object_set_order) of the root's children and
// of the machine node's children, in one table.
//
// Siblings sort by (order, attach sequence), so two siblings that share a
// weight fall to whichever was attached first -- an order that a checkpoint
// load, which builds the nodes in another sequence, need not reproduce.
// Every node that can sit beside another under the root or the machine
// node therefore takes its weight from here, and no two such nodes share
// one.  Children inside one device keep local small numbers (a card's fb /
// monitor / regs / dac); those never meet siblings from elsewhere.
//
// The root's order is asserted by integration object-model-reorg, and the
// machine's is what the web frontend's SYSTEM tab shows: a renumbering must
// keep the sequence.

#ifndef OBJECT_ORDER_H
#define OBJECT_ORDER_H

// Children of the root (object_root()), in display order
enum {
    OBJ_ORDER_MACHINE = 0, // the machine sorts first
    OBJ_ORDER_SCHEDULER = 10,
    OBJ_ORDER_CHECKPOINT = 20,
    OBJ_ORDER_PACING = 21,
    OBJ_ORDER_PREDECODE = 22,
    OBJ_ORDER_JIT = 23,
    OBJ_ORDER_FILES = 30,
    OBJ_ORDER_DEBUG = 40,
    OBJ_ORDER_LOG = 50,
    OBJ_ORDER_SHELL = 60,
    OBJ_ORDER_CATALOG = 70,
    OBJ_ORDER_APPLETALK = 100,
};

// Children of the machine node (machine_object()), in display order.  The
// printers (imagewriter) keep the default weight 0 and sort first.  A
// family's node of a given role (its I/O controller, its floppy) takes the
// role's weight: no machine has two.
enum {
    OBJ_ORDER_CPU = 10,
    OBJ_ORDER_DSP = 15, // the AV boards' DSP3210
    OBJ_ORDER_MEMORY = 20,
    OBJ_ORDER_ROM = 30,
    OBJ_ORDER_VIA = 40, // via1 and via2 share it: the same node kind, attached in index order
    OBJ_ORDER_HOST_BRIDGE = 44, // grackle
    OBJ_ORDER_IO_CONTROLLER = 45, // rbv, oss, gc, amic, heathrow, psc
    OBJ_ORDER_NVRAM = 46,
    OBJ_ORDER_SCC = 50,
    OBJ_ORDER_RTC = 60,
    OBJ_ORDER_ADB = 70,
    OBJ_ORDER_FLOPPY = 80,
    OBJ_ORDER_SCSI = 90, // the primary bus (machine.scsi)
    OBJ_ORDER_SCSI_SECONDARY = 91, // scsi2, the Gossamer's atapi bus
    OBJ_ORDER_PROFILE = 92, // the Lisa's ProFile (hd)
    OBJ_ORDER_ATA = 93,
    OBJ_ORDER_ETHERNET = 94, // bmac
    OBJ_ORDER_NUBUS = 100,
    OBJ_ORDER_PCI = 101,
    OBJ_ORDER_SOUND = 110,
    OBJ_ORDER_SCREEN = 120,
    OBJ_ORDER_BOARD = 121, // the ANS's board node (keyswitch, power supplies)
    OBJ_ORDER_LCD = 122, // the ANS's front-panel LCD
    OBJ_ORDER_VIDEO = 123, // the on-board display
    OBJ_ORDER_VIDEO_IN = 125, // the AV boards' video input (vdc)
    OBJ_ORDER_AUDIO_IN = 126, // the AV boards' audio input
    OBJ_ORDER_POWER = 130, // the Lisa's soft power switch
};

#endif // OBJECT_ORDER_H
