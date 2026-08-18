// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// test.c — unit tests for the PPC (MPC601) disassembler
// (src/core/cpu/ppc/ppc_disasm.c).
//
// Vector sources:
//   1. Real words from the PDM boot ROM's HWInit/nanokernel region
//      (addresses in the $FFFxxxxx rows), spot-checked against the PDM
//      dossier's published fragments — e.g. the reset vector's
//      `b $FFF03000` into HWInit.
//   2. Directed encodings across the integer/POWER/branch/SPR/FP surface.
//   3. Encodings that must NOT decode on a 601: mftb, fsel (603+),
//      tlbld (603), invalid-form words with reserved fields set.
//
// Every expected string was cross-validated against
// `powerpc-linux-gnu-objdump -b binary -m powerpc:601 -EB` during
// development, modulo the documented simplified-mnemonic differences
// (objdump's mtsprg/mfdec-style aliases, POWER fallback spellings, and
// its stricter reserved-field rejection of invalid forms).

#include "ppc_disasm.h"

#include <stdio.h>
#include <string.h>

static int failures, checks;

// Expected text uses ' ' where the disassembler emits '\t'.
static void expect(uint32_t addr, uint32_t word, const char *want) {
    ppc_insn ins;
    ppc_disassemble(word, addr, &ins);
    char got[sizeof(ins.text)];
    snprintf(got, sizeof(got), "%s", ins.text);
    for (char *c = got; *c; c++)
        if (*c == '\t')
            *c = ' ';
    checks++;
    if (strcmp(got, want) != 0) {
        printf("FAIL %08X: want \"%s\" got \"%s\"\n", word, want, got);
        failures++;
        return;
    }
    // Status must agree with the text form.
    int want_invalid = strncmp(want, ".long", 5) == 0;
    if (want_invalid != (ins.status == PPC_DIS_INVALID)) {
        printf("FAIL %08X: status %d inconsistent with \"%s\"\n", word, ins.status, want);
        failures++;
    }
}

static const struct {
    uint32_t addr;
    uint32_t word;
    const char *text;
} vectors[] = {
    {0xFFF00100u, 0x48002F00u, "b $FFF03000"            },
    {0xFFF03000u, 0x38000000u, "li r0,0"                },
    {0xFFF03008u, 0x48000355u, "bl $FFF0335C"           },
    {0xFFF0300Cu, 0x40820FF1u, "bnel $FFF03FFC"         },
    {0xFFF03020u, 0x601D0000u, "ori r29,r0,0"           },
    {0xFFF03024u, 0x67BD3BC4u, "oris r29,r29,15300"     },
    {0xFFF0302Cu, 0x7C7C1B78u, "mr r28,r3"              },
    {0xFFF00000u, 0x7C3143A6u, "mtspr sprg1,r1"         },
    {0xFFF0000Cu, 0x7C3342A6u, "mfspr r1,sprg3"         },
    {0xFFF0103Cu, 0x7C7B02A6u, "mfspr r3,srr1"          },
    {0xFFF03548u, 0x7C1183A6u, "mtspr ibat0l,r0"        },
    {0xFFF0415Cu, 0x7C8402A6u, "mfspr r4,rtcu"          },
    {0xFFF04160u, 0x7CA502A6u, "mfspr r5,rtcl"          },
    {0xFFF034ACu, 0x7C7F42A6u, "mfspr r3,pvr"           },
    {0xFFF03678u, 0x7CB603A6u, "mtspr dec,r5"           },
    {0xFFF03E44u, 0x7CF902A6u, "mfspr r7,sdr1"          },
    {0xFFF03570u, 0x7C70FAA6u, "mfspr r3,hid0"          },
    {0xFFF10F30u, 0x7ED6B0F8u, "nor r22,r22,r22"        },
    {0x00001000u, 0x7C642A14u, "add r3,r4,r5"           },
    {0x00001000u, 0x7C642A15u, "add. r3,r4,r5"          },
    {0x00001000u, 0x7C646E15u, "addo. r3,r4,r13"        },
    {0x00001000u, 0x7C642E14u, "addo r3,r4,r5"          },
    {0x00001000u, 0x7C642810u, "subfc r3,r4,r5"         },
    {0x00001000u, 0x7C642914u, "adde r3,r4,r5"          },
    {0x00001000u, 0x7C6429D4u, ".long $7C6429D4"        },
    {0x00001000u, 0x7C642994u, ".long $7C642994"        },
    {0x00001000u, 0x7C640194u, "addze r3,r4"            },
    {0x00001000u, 0x7C6401D4u, "addme r3,r4"            },
    {0x00001000u, 0x7C6400D0u, "neg r3,r4"              },
    {0x00001000u, 0x7C642896u, "mulhw r3,r4,r5"         },
    {0x00001000u, 0x7C642816u, "mulhwu r3,r4,r5"        },
    {0x00001000u, 0x7C6429D6u, "mullw r3,r4,r5"         },
    {0x00001000u, 0x7C642BD6u, "divw r3,r4,r5"          },
    {0x00001000u, 0x7C642B96u, "divwu r3,r4,r5"         },
    {0x00001000u, 0x7C6402D0u, "abs r3,r4"              },
    {0x00001000u, 0x7C6403D0u, "nabs r3,r4"             },
    {0x00001000u, 0x7C642A10u, "doz r3,r4,r5"           },
    {0x00001000u, 0x7C6428D6u, "mul r3,r4,r5"           },
    {0x00001000u, 0x7C642A96u, "div r3,r4,r5"           },
    {0x00001000u, 0x7C642AD6u, "divs r3,r4,r5"          },
    {0x00001000u, 0x7C642A2Au, "lscbx r3,r4,r5"         },
    {0x00001000u, 0x7C64283Au, "maskg r4,r3,r5"         },
    {0x00001000u, 0x7C642C3Au, "maskir r4,r3,r5"        },
    {0x00001000u, 0x7C642C32u, "rrib r4,r3,r5"          },
    {0x00001000u, 0x7C642932u, "sle r4,r3,r5"           },
    {0x00001000u, 0x7C6429B2u, "sleq r4,r3,r5"          },
    {0x00001000u, 0x7C642970u, "sliq r4,r3,5"           },
    {0x00001000u, 0x7C642B70u, ".long $7C642B70"        },
    {0x00001000u, 0x7C6429B0u, "sllq r4,r3,r5"          },
    {0x00001000u, 0x7C642930u, "slq r4,r3,r5"           },
    {0x00001000u, 0x7C642D30u, "srq r4,r3,r5"           },
    {0x00001000u, 0x7C642D32u, "sre r4,r3,r5"           },
    {0x00001000u, 0x7C642DF0u, "srliq r4,r3,5"          },
    {0x00001000u, 0x7C642F38u, ".long $7C642F38"        },
    {0x00001000u, 0x7C642E70u, "srawi r4,r3,5"          },
    {0x00001000u, 0x7C642D70u, "sriq r4,r3,5"           },
    {0x00001000u, 0x7C642DB2u, "sreq r4,r3,r5"          },
    {0x00001000u, 0x7C642F70u, "sraiq r4,r3,5"          },
    {0x00001000u, 0x7C642732u, "srea r4,r3,r4"          },
    {0x00001000u, 0x1C650040u, "mulli r3,r5,64"         },
    {0x00001000u, 0x24650040u, "dozi r3,r5,64"          },
    {0x00001000u, 0x7C641838u, "and r4,r3,r3"           },
    {0x00001000u, 0x7C641839u, "and. r4,r3,r3"          },
    {0x00001000u, 0x5464103Au, "rlwinm r4,r3,2,0,29"    },
    {0x00001000u, 0x5464A33Eu, "rlwinm r4,r3,20,12,31"  },
    {0x00001000u, 0x50642D74u, "rlwimi r4,r3,5,21,26"   },
    {0x00001000u, 0x5C642D74u, "rlwnm r4,r3,r5,21,26"   },
    {0x00001000u, 0x7C642E30u, "sraw r4,r3,r5"          },
    {0x00001000u, 0x7C642E31u, "sraw. r4,r3,r5"         },
    {0x00001000u, 0x7C642670u, "srawi r4,r3,4"          },
    {0x00001000u, 0x7C6428F8u, "nor r4,r3,r5"           },
    {0x00001000u, 0x7C640774u, "extsb r4,r3"            },
    {0x00001000u, 0x7C640734u, "extsh r4,r3"            },
    {0x00001000u, 0x7C640034u, "cntlzw r4,r3"           },
    {0x00001000u, 0x38600010u, "li r3,16"               },
    {0x00001000u, 0x3C608000u, "lis r3,-32768"          },
    {0x00001000u, 0x38830010u, "addi r4,r3,16"          },
    {0x00001000u, 0x60000000u, "nop"                    },
    {0x00001000u, 0x60641234u, "ori r4,r3,4660"         },
    {0x00001000u, 0x70641234u, "andi. r4,r3,4660"       },
    {0x00001000u, 0x2C830005u, "cmpwi cr1,r3,5"         },
    {0x00001000u, 0x28030005u, "cmplwi cr0,r3,5"        },
    {0x00002000u, 0x48000355u, "bl $00002354"           },
    {0x00002000u, 0x4BFFFFF0u, "b $00001FF0"            },
    {0x00002000u, 0x48000002u, "ba $00000000"           },
    {0x00002000u, 0x4E800020u, "blr"                    },
    {0x00002000u, 0x4E800021u, "blrl"                   },
    {0x00002000u, 0x4E800420u, "bctr"                   },
    {0x00002000u, 0x41820010u, "beq $00002010"          },
    {0x00002000u, 0x4082FFF0u, "bne $00001FF0"          },
    {0x00002000u, 0x419D0010u, "bgt cr7,$00002010"      },
    {0x00002000u, 0x42400010u, "bdz $00002010"          },
    {0x00002000u, 0x42000010u, "bdnz $00002010"         },
    {0x00002000u, 0x4D820020u, "beqlr"                  },
    {0x00002000u, 0x40810010u, "ble $00002010"          },
    {0x00002000u, 0x41800010u, "blt $00002010"          },
    {0x00002000u, 0x4C000064u, "rfi"                    },
    {0x00002000u, 0x44000002u, "sc"                     },
    {0x00001000u, 0x4C221182u, "crxor 1,2,2"            },
    {0x00001000u, 0x4C221202u, "crand 1,2,2"            },
    {0x00001000u, 0x4CC63182u, "crxor 6,6,6"            },
    {0x00001000u, 0xFC00048Eu, "mffs f0"                },
    {0x00001000u, 0x7C600026u, "mfcr r3"                },
    {0x00001000u, 0x7C6FF120u, "mtcrf 255,r3"           },
    {0x00001000u, 0x7C000400u, "mcrxr cr0"              },
    {0x00001000u, 0x7C0004ACu, "sync"                   },
    {0x00001000u, 0x7C0006ACu, "eieio"                  },
    {0x00001000u, 0x4C00012Cu, "isync"                  },
    {0x00001000u, 0x7C7002A6u, "mfspr r3,16"            },
    {0x00001000u, 0x7C9043A6u, "mtspr sprg0,r4"         },
    {0x00001000u, 0x7C6000A6u, "mfmsr r3"               },
    {0x00001000u, 0x7C600124u, "mtmsr r3"               },
    {0x00001000u, 0x7C7042A6u, "mfspr r3,sprg0"         },
    {0x00001000u, 0x80640010u, "lwz r3,16(r4)"          },
    {0x00001000u, 0x84640010u, "lwzu r3,16(r4)"         },
    {0x00001000u, 0x88640010u, "lbz r3,16(r4)"          },
    {0x00001000u, 0x90640010u, "stw r3,16(r4)"          },
    {0x00001000u, 0xA0640010u, "lhz r3,16(r4)"          },
    {0x00001000u, 0xA8640010u, "lha r3,16(r4)"          },
    {0x00001000u, 0xB0640010u, "sth r3,16(r4)"          },
    {0x00001000u, 0xB8640010u, "lmw r3,16(r4)"          },
    {0x00001000u, 0xBC640010u, "stmw r3,16(r4)"         },
    {0x00001000u, 0x7C64182Eu, "lwzx r3,r4,r3"          },
    {0x00001000u, 0x7C64196Eu, "stwux r3,r4,r3"         },
    {0x00001000u, 0x7C641A2Eu, "lhzx r3,r4,r3"          },
    {0x00001000u, 0x7C641C2Cu, "lwbrx r3,r4,r3"         },
    {0x00001000u, 0x7C641D2Cu, "stwbrx r3,r4,r3"        },
    {0x00001000u, 0x7C640028u, "lwarx r3,r4,r0"         },
    {0x00001000u, 0x7C64012Du, "stwcx. r3,r4,r0"        },
    {0x00001000u, 0x7C64052Au, "stswx r3,r4,r0"         },
    {0x00001000u, 0x7C6404AAu, "lswi r3,r4,32"          },
    {0x00001000u, 0x7C6405AAu, "stswi r3,r4,32"         },
    {0x00001000u, 0x7C00206Cu, "dcbst 0,r4"             },
    {0x00001000u, 0x7C0027ACu, "icbi 0,r4"              },
    {0x00001000u, 0x7C001FECu, "dcbz 0,r3"              },
    {0x00001000u, 0xC0640010u, "lfs f3,16(r4)"          },
    {0x00001000u, 0xC8640010u, "lfd f3,16(r4)"          },
    {0x00001000u, 0xD0640010u, "stfs f3,16(r4)"         },
    {0x00001000u, 0xD8640010u, "stfd f3,16(r4)"         },
    {0x00001000u, 0x7C64252Eu, "stfsx f3,r4,r4"         },
    {0x00001000u, 0x7C6425AEu, "stfdx f3,r4,r4"         },
    {0x00001000u, 0xFC601890u, "fmr f3,f3"              },
    {0x00001000u, 0xFC601850u, "fneg f3,f3"             },
    {0x00001000u, 0xFC601A10u, "fabs f3,f3"             },
    {0x00001000u, 0xFC601910u, "fnabs f3,f3"            },
    {0x00001000u, 0xFC032000u, "fcmpu cr0,f3,f4"        },
    {0x00001000u, 0xFC032040u, "fcmpo cr0,f3,f4"        },
    {0x00001000u, 0xFC60048Eu, "mffs f3"                },
    {0x00001000u, 0xFDFE058Eu, "mtfsf 255,f0"           },
    {0x00001000u, 0x7C6C42E6u, ".long $7C6C42E6"        },
    {0x00001000u, 0xFC4D34EEu, ".long $FC4D34EE"        },
    {0x00001000u, 0x7C0007A4u, ".long $7C0007A4"        },
    {0x00001000u, 0x00000000u, ".long $00000000"        },
    {0x00001000u, 0xFFFFFFFFu, "fnmadd. f31,f31,f31,f31"},
};

// Branch-target metadata checks
static void test_targets(void) {
    ppc_insn ins;
    ppc_disassemble(0x48002F00u, 0xFFF00100u, &ins); // reset vector -> HWInit
    checks++;
    if (!ins.is_branch || !ins.has_target || ins.target != 0xFFF03000u) {
        printf("FAIL: reset-vector target %08X\n", ins.target);
        failures++;
    }
    ppc_disassemble(0x4082FFF0u, 0x2000u, &ins); // bne backwards
    checks++;
    if (!ins.has_target || ins.target != 0x1FF0u) {
        printf("FAIL: bne target %08X\n", ins.target);
        failures++;
    }
}

// POWER-holdover flagging
static void test_power_flag(void) {
    ppc_insn ins;
    ppc_disassemble(0x7C6402D0u, 0x1000u, &ins); // abs
    checks++;
    if (!ins.is_power) {
        printf("FAIL: abs not flagged as POWER\n");
        failures++;
    }
    ppc_disassemble(0x7C642A14u, 0x1000u, &ins); // add
    checks++;
    if (ins.is_power) {
        printf("FAIL: add flagged as POWER\n");
        failures++;
    }
}

int main(void) {
    for (unsigned i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++)
        expect(vectors[i].addr, vectors[i].word, vectors[i].text);
    test_targets();
    test_power_flag();
    printf("ppc_disasm: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
