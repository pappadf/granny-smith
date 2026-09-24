// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// log_categories.h
// The complete set of log categories, in one place.
//
// Before this existed a category came into being four different ways --
// LOG_USE_CATEGORY_NAME's lazy register on first hit, explicit
// log_register_category calls, and log_configure creating whatever name it
// was handed -- and there was no manifest.  Three consequences, all of which
// this batch hit:
//
//   * `debug.log cpuu 10` succeeded, reported a configured `cpuu`, and
//     produced no output and no error: a silent dead end for anyone
//     debugging (F-34).
//   * `debug.log` with no arguments listed only categories that had already
//     been HIT or configured, so it could not be used to discover the right
//     name either.
//   * The checkpoint path logged under three different names -- `ckpt`,
//     `checkpoint` and `setup` -- and nothing could tell, because nothing
//     knew what the real set was.
//
// Adding a category means adding a row here.  LOG_USE_CATEGORY_NAME and
// log_configure both validate against this table, so a typo in either is an
// error at the point it is made rather than silence later.

#ifndef GS_LOG_CATEGORIES_H
#define GS_LOG_CATEGORIES_H

// X(name, default_level, description)
//
// Default level is 0 for everything today; the column exists so a category
// that should be on out of the box can say so in the table rather than in
// start-up code.
#define GS_LOG_CATEGORIES(X)                                                                                           \
    /* CPU, memory and the execution core */                                                                           \
    X("cpu", 0, "68K instruction execution and exceptions")                                                            \
    X("fpu", 0, "68881/68882 floating point")                                                                          \
    X("mmu", 0, "68030/68040 MMU translation and ATC")                                                                 \
    X("memory", 0, "Address decode and the SoA fast path")                                                             \
    X("ppc", 0, "PowerPC execution")                                                                                   \
    X("ppcmmu", 0, "PowerPC BAT/segment translation")                                                                  \
    X("dsp3210", 0, "AT&T DSP3210 on the AV machines")                                                                 \
    X("scheduler", 0, "Event queue and pacing")                                                                        \
    /* Storage and media */                                                                                            \
    X("floppy", 0, "IWM/SWIM/SWIM3 controllers and disk images")                                                       \
    X("scsi", 0, "SCSI bus and devices")                                                                               \
    X("53c96", 0, "NCR 53C96 controller")                                                                              \
    X("53c825", 0, "Symbios 53C825 PCI controller")                                                                    \
    X("mesh", 0, "MESH SCSI controller")                                                                               \
    X("scripts", 0, "53C8xx SCRIPTS engine")                                                                           \
    X("image", 0, "Disk image open/close and geometry")                                                                \
    X("storage", 0, "Delta/journal storage engine")                                                                    \
    X("profile", 0, "Lisa ProFile hard disk")                                                                          \
    /* Input, RTC and companion controllers */                                                                         \
    X("adb", 0, "Apple Desktop Bus transactions")                                                                      \
    X("keyboard", 0, "Keyboard input and key mapping")                                                                 \
    X("mouse", 0, "Mouse input")                                                                                       \
    X("cops", 0, "Lisa COPS keyboard/mouse controller")                                                                \
    X("cuda", 0, "Cuda companion MCU")                                                                                 \
    X("egret", 0, "Egret companion MCU")                                                                               \
    X("rtc", 0, "Real-time clock and PRAM")                                                                            \
    X("via", 0, "VIA 6522 ports, timers and shift register")                                                           \
    /* Serial, network and sound */                                                                                    \
    X("scc", 0, "Z8530 serial controller")                                                                             \
    X("appletalk", 0, "AppleTalk stack: DDP, NBP, configuration")                                                      \
    X("llap", 0, "AppleTalk LLAP link and node address")                                                               \
    X("atp", 0, "AppleTalk ATP transactions")                                                                          \
    X("asp", 0, "AppleTalk ASP sessions")                                                                              \
    X("afp", 0, "AFP file server")                                                                                     \
    X("pap", 0, "AppleTalk PAP printer server")                                                                        \
    X("laserwriter", 0, "LaserWriter interpreter bridge")                                                              \
    X("adsp", 0, "AppleTalk ADSP connections")                                                                         \
    X("ppctoolbox", 0, "AppleTalk PPC Toolbox program linking")                                                        \
    X("aevt", 0, "Apple events over PPC")                                                                              \
    X("sonic", 0, "SONIC Ethernet controller")                                                                         \
    X("mace", 0, "MACE Ethernet controller")                                                                           \
    X("sound", 0, "Sound output path")                                                                                 \
    X("asc", 0, "Apple Sound Chip")                                                                                    \
    X("audio", 0, "Host audio output and capture")                                                                     \
    X("awacs", 0, "AWACS audio codec")                                                                                 \
    X("singer", 0, "Singer audio codec on the AV machines")                                                            \
    /* Video */                                                                                                        \
    X("video", 0, "Display models, framebuffers and scanout")                                                          \
    X("voodoo2", 0, "3dfx Voodoo2 accelerator")                                                                        \
    X("vdc", 0, "AV video display controller")                                                                         \
    X("lcd", 0, "PowerBook/ANS LCD panel")                                                                             \
    /* Buses and chipsets */                                                                                           \
    X("nubus", 0, "NuBus slots and declaration ROMs")                                                                  \
    X("pci", 0, "PCI configuration and BAR decode")                                                                    \
    X("bandit", 0, "Bandit PCI bridge")                                                                                \
    X("gc", 0, "Grand Central I/O controller")                                                                         \
    X("hammerhead", 0, "Hammerhead memory controller")                                                                 \
    X("hmc", 0, "HMC memory controller")                                                                               \
    X("bart", 0, "BART DMA/bus controller")                                                                            \
    X("amic", 0, "AMIC I/O controller")                                                                                \
    X("dbdma", 0, "Descriptor-based DMA")                                                                              \
    X("gbus", 0, "ANS GBus")                                                                                           \
    X("new_age", 0, "New Age floppy/IO controller")                                                                    \
    X("psc", 0, "PSC DMA controller")                                                                                  \
    X("rbv", 0, "RBV video/interrupt controller")                                                                      \
    X("iop", 0, "I/O Processor core")                                                                                  \
    X("iop_scc", 0, "SCC I/O Processor firmware")                                                                      \
    X("iop_swim", 0, "SWIM I/O Processor firmware")                                                                    \
    /* ROM, machine construction and tooling */                                                                        \
    X("rom", 0, "ROM identification and loading")                                                                      \
    X("vrom", 0, "Video declaration ROMs")                                                                             \
    X("prom", 0, "Boot/parameter ROM")                                                                                 \
    X("board", 0, "Machine construction and teardown")                                                                 \
    X("setup", 0, "System setup and media attachment")                                                                 \
    X("ckpt", 0, "Checkpoint save and restore")                                                                        \
    X("alias", 0, "Shell alias table")                                                                                 \
    /* Debug surfaces, declared in log.h rather than by a module */                                                    \
    X("logpoint", 0, "Memory and PC logpoints")                                                                        \
    X("exceptions", 0, "Guest exception tracing")

#endif // GS_LOG_CATEGORIES_H
