// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// ppc_spr.h
// The special-purpose register table: every SPR number the 601, 604 and
// 750 decode, as one X-macro list the SPR's users expand:
//   - ppc_mfspr / ppc_mtspr (ppc.c): a switch arm per number, generated for
//     the plain directions, written by hand for the ones with side effects;
//   - the translator's classifier (ppc_jit_classify.h): the SPRs it moves;
//   - the disassembler's SPR names (ppc_disasm.c);
//   - the `machine.cpu` register attributes (ppc.c, ppc_attr_slot).
// Like ppc_fields.h it depends on nothing, so the standalone disassembler
// build can include it.
//
// X(n, name, mf, mt, field, flags, models):
//   n       the SPR number (the two instruction halves already swapped)
//   name    the disassembler's name, or NULL to print the number
//   mf, mt  what mfspr / mtspr do with the number:
//             PLAIN  read / write `field`, after the model and privilege
//                    gates the table states (generated);
//             OWN    an arm of its own in ppc.c (side effects, or a value
//                    that is not the field), which applies the table's
//                    gates itself;
//             NO     not decoded that way: the undefined-SPR path
//   field   the ppc_t member that holds the register, `none` where nothing
//           does (only PLAIN directions expand it)
//   flags   PPC_SPR_PRIV: supervisor-only; PPC_SPR_JIT: the translator
//           moves it (which the classifier requires to be supervisor, PLAIN
//           both ways, on every model)
//   models  the models that have it: PPC_SPR_M601 / M604 (the 604 proper)
//           / M750, or PPC_SPR_MALL
// The ppc_750 unit suite (test_spr_table) checks every row's models,
// directions and privilege against what mfspr and mtspr do on the 604
// and the 750.

#ifndef PPC_SPR_H
#define PPC_SPR_H

#include "ppc_fields.h"

#include <stddef.h> // NULL

#define PPC_SPR_PRIV 1u
#define PPC_SPR_JIT  2u

#define PPC_SPR_M601 1u
#define PPC_SPR_M604 2u
#define PPC_SPR_M750 4u
#define PPC_SPR_MALL (PPC_SPR_M601 | PPC_SPR_M604 | PPC_SPR_M750)
#define PPC_SPR_M6XX (PPC_SPR_M604 | PPC_SPR_M750) // the 604-class models

#define SPR_U  0u
#define SPR_S  PPC_SPR_PRIV
#define SPR_SJ (PPC_SPR_PRIV | PPC_SPR_JIT)

// clang-format off
#define PPC_SPRS(X) \
    X(0,    "mq",     PLAIN, PLAIN, mq,      SPR_U,  PPC_SPR_M601) /* POWER holdover */ \
    X(1,    "xer",    PLAIN, OWN,   xer,     SPR_U,  PPC_SPR_MALL) /* mt: the 601 keeps the implemented bits */ \
    X(4,    "rtcu",   OWN,   NO,    rtcu,    SPR_U,  PPC_SPR_M601) /* RTC read encodings, every mode */ \
    X(5,    "rtcl",   OWN,   NO,    rtcl,    SPR_U,  PPC_SPR_M601) \
    X(6,    "dec",    OWN,   NO,    dec,     SPR_U,  PPC_SPR_M601) /* POWER user-level DEC read */ \
    X(8,    "lr",     PLAIN, PLAIN, lr,      SPR_U,  PPC_SPR_MALL) \
    X(9,    "ctr",    PLAIN, PLAIN, ctr,     SPR_U,  PPC_SPR_MALL) \
    X(18,   "dsisr",  PLAIN, PLAIN, dsisr,   SPR_SJ, PPC_SPR_MALL) \
    X(19,   "dar",    PLAIN, PLAIN, dar,     SPR_SJ, PPC_SPR_MALL) \
    X(20,   "rtcu",   NO,    OWN,   rtcu,    SPR_S,  PPC_SPR_M601) /* RTC write encodings */ \
    X(21,   "rtcl",   NO,    OWN,   rtcl,    SPR_S,  PPC_SPR_M601) \
    X(22,   "dec",    OWN,   OWN,   dec,     SPR_S,  PPC_SPR_MALL) /* live count; mt re-arms */ \
    X(25,   "sdr1",   PLAIN, OWN,   sdr1,    SPR_S,  PPC_SPR_MALL) /* mt: the HTAB moved */ \
    X(26,   "srr0",   PLAIN, PLAIN, srr0,    SPR_SJ, PPC_SPR_MALL) \
    X(27,   "srr1",   PLAIN, PLAIN, srr1,    SPR_SJ, PPC_SPR_MALL) \
    X(268,  NULL,     OWN,   NO,    none,    SPR_U,  PPC_SPR_M6XX) /* TBL/TBU through mfspr (named by mftb) */ \
    X(269,  NULL,     OWN,   NO,    none,    SPR_U,  PPC_SPR_M6XX) \
    X(272,  "sprg0",  PLAIN, PLAIN, sprg[0], SPR_SJ, PPC_SPR_MALL) \
    X(273,  "sprg1",  PLAIN, PLAIN, sprg[1], SPR_SJ, PPC_SPR_MALL) \
    X(274,  "sprg2",  PLAIN, PLAIN, sprg[2], SPR_SJ, PPC_SPR_MALL) \
    X(275,  "sprg3",  PLAIN, PLAIN, sprg[3], SPR_SJ, PPC_SPR_MALL) \
    X(282,  "ear",    PLAIN, PLAIN, ear,     SPR_S,  PPC_SPR_MALL) \
    X(284,  "tbl",    NO,    OWN,   none,    SPR_S,  PPC_SPR_M6XX) /* time base write encodings */ \
    X(285,  "tbu",    NO,    OWN,   none,    SPR_S,  PPC_SPR_M6XX) \
    X(287,  "pvr",    PLAIN, NO,    pvr,     SPR_S,  PPC_SPR_MALL) \
    X(528,  "ibat0u", OWN,   OWN,   batu[0], SPR_S,  PPC_SPR_MALL) /* 601 unified BATs / 604 IBATs */ \
    X(529,  "ibat0l", OWN,   OWN,   batl[0], SPR_S,  PPC_SPR_MALL) \
    X(530,  "ibat1u", OWN,   OWN,   batu[1], SPR_S,  PPC_SPR_MALL) \
    X(531,  "ibat1l", OWN,   OWN,   batl[1], SPR_S,  PPC_SPR_MALL) \
    X(532,  "ibat2u", OWN,   OWN,   batu[2], SPR_S,  PPC_SPR_MALL) \
    X(533,  "ibat2l", OWN,   OWN,   batl[2], SPR_S,  PPC_SPR_MALL) \
    X(534,  "ibat3u", OWN,   OWN,   batu[3], SPR_S,  PPC_SPR_MALL) \
    X(535,  "ibat3l", OWN,   OWN,   batl[3], SPR_S,  PPC_SPR_MALL) \
    X(536,  "dbat0u", OWN,   OWN,   dbatu[0], SPR_S, PPC_SPR_M6XX) /* the 604's split file */ \
    X(537,  "dbat0l", OWN,   OWN,   dbatl[0], SPR_S, PPC_SPR_M6XX) \
    X(538,  "dbat1u", OWN,   OWN,   dbatu[1], SPR_S, PPC_SPR_M6XX) \
    X(539,  "dbat1l", OWN,   OWN,   dbatl[1], SPR_S, PPC_SPR_M6XX) \
    X(540,  "dbat2u", OWN,   OWN,   dbatu[2], SPR_S, PPC_SPR_M6XX) \
    X(541,  "dbat2l", OWN,   OWN,   dbatl[2], SPR_S, PPC_SPR_M6XX) \
    X(542,  "dbat3u", OWN,   OWN,   dbatu[3], SPR_S, PPC_SPR_M6XX) \
    X(543,  "dbat3l", OWN,   OWN,   dbatl[3], SPR_S, PPC_SPR_M6XX) \
    X(936,  "ummcr0", PLAIN, NO,    mmcr[0], SPR_U,  PPC_SPR_M750) /* 750 user-level monitor mirrors (750UM Table 2-49) */ \
    X(937,  "upmc1",  PLAIN, NO,    pmc[0],  SPR_U,  PPC_SPR_M750) \
    X(938,  "upmc2",  PLAIN, NO,    pmc[1],  SPR_U,  PPC_SPR_M750) \
    X(939,  "usia",   PLAIN, NO,    sia,     SPR_U,  PPC_SPR_M750) \
    X(940,  "ummcr1", PLAIN, NO,    mmcr[1], SPR_U,  PPC_SPR_M750) \
    X(941,  "upmc3",  PLAIN, NO,    pmc[2],  SPR_U,  PPC_SPR_M750) \
    X(942,  "upmc4",  PLAIN, NO,    pmc[3],  SPR_U,  PPC_SPR_M750) \
    X(952,  "mmcr0",  OWN,   OWN,   mmcr[0], SPR_S,  PPC_SPR_M6XX) /* 604: read-zero stubs; 750: stored */ \
    X(953,  "pmc1",   OWN,   OWN,   pmc[0],  SPR_S,  PPC_SPR_M6XX) \
    X(954,  "pmc2",   OWN,   OWN,   pmc[1],  SPR_S,  PPC_SPR_M6XX) \
    X(955,  "sia",    OWN,   OWN,   sia,     SPR_S,  PPC_SPR_M6XX) \
    X(956,  "mmcr1",  PLAIN, PLAIN, mmcr[1], SPR_S,  PPC_SPR_M750) /* the 750's additions to the monitor */ \
    X(957,  "pmc3",   PLAIN, PLAIN, pmc[2],  SPR_S,  PPC_SPR_M750) \
    X(958,  "pmc4",   PLAIN, PLAIN, pmc[3],  SPR_S,  PPC_SPR_M750) \
    X(959,  "sda",    OWN,   OWN,   none,    SPR_S,  PPC_SPR_M604) /* 604 read-zero stub (no storage) */ \
    X(1008, "hid0",   PLAIN, OWN,   hid0,    SPR_S,  PPC_SPR_MALL) /* mt: the 750's flash-invalidate bits */ \
    X(1009, "hid1",   PLAIN, OWN,   hid1,    SPR_S,  PPC_SPR_M601 | PPC_SPR_M750) /* mt: read-only on the 750 */ \
    X(1010, "iabr",   PLAIN, PLAIN, iabr,    SPR_S,  PPC_SPR_MALL) \
    X(1013, "dabr",   PLAIN, PLAIN, dabr,    SPR_S,  PPC_SPR_MALL) \
    X(1017, "l2cr",   OWN,   OWN,   l2cr,    SPR_S,  PPC_SPR_M750) /* L2IP, the invalidate's timing */ \
    X(1019, "ictc",   PLAIN, PLAIN, ictc,    SPR_S,  PPC_SPR_M750) \
    X(1020, "thrm1",  OWN,   OWN,   thrm[0], SPR_S,  PPC_SPR_M750) /* TIN/TIV derived */ \
    X(1021, "thrm2",  OWN,   OWN,   thrm[1], SPR_S,  PPC_SPR_M750) \
    X(1022, "thrm3",  OWN,   OWN,   thrm[2], SPR_S,  PPC_SPR_M750) \
    X(1023, "pir",    PLAIN, PLAIN, pir,     SPR_S,  PPC_SPR_M601 | PPC_SPR_M604)
// clang-format on

#endif // PPC_SPR_H
