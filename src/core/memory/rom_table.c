// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// rom_table.c
// Every CPU ROM Granny Smith knows about, keyed by content id (rom.h).  A
// row's compatible list names the emulated models it boots; NONE marks a
// real ROM for a machine that is not emulated, which rom.identify reports as
// recognised but not supported.  Adding a machine model means adding its id
// to the compatible lists of the ROMs it runs.
//
// Rows come from images we hold and have verified, from a published list of
// ROM checksum fields, or from published checksum values alone; the row
// names are ours.  Canonical fixture filenames are not a core concern.

#include "rom.h"

#include <stddef.h>

// === Compatible-model lists ================================================

// A ROM for a machine that is not emulated.
static const char *const NONE[] = {NULL};
static const char *const PLUS[] = {"plus", NULL};
// Universal ROM: same bytes for IIx, IIcx, SE/30. Listed in machine_register
// order so the *first* entry serves as a UI default (for tests that don't pick
// explicitly), but the API never auto-selects.
static const char *const UNIVERSAL[] = {"se30", "iicx", "iix", NULL};
static const char *const IIFX[] = {"iifx", NULL};
// Dedicated 512 KB Macintosh IIci ("Aurora") ROM — not the universal ROM.
static const char *const IICI[] = {"iici", NULL};
// Dedicated 512 KB Macintosh IIsi ("Erickson") ROM — not the universal ROM.
static const char *const IISI[] = {"iisi", NULL};
// Quadra 700/900 shared 1 MB ROM (also shipped in the PowerBook 140/170 —
// portables are out of scope and not listed as targets).
static const char *const Q700_Q900[] = {"q700", "q900", NULL};
// Dedicated Quadra 950 ("Zydeco") 1 MB ROM.
static const char *const Q950[] = {"q950", NULL};
// Quadra 840AV / Centris 660AV shared 2 MB "Cyclone/Tempest" ROM.
static const char *const AV[] = {"q840av", "q660av", NULL};
// Power Macintosh 6100/7100/8100 shared 4 MB "Boot PDM 601 1.0" ROM.
static const char *const PDM[] = {"pm6100", "pm7100", "pm8100", NULL};
// The later "Boot PDM 601 1.1" ROM, listed for the 7100 only until it is
// known which other models shipped it.
static const char *const PM7100[] = {"pm7100", NULL};
// Power Macintosh 7500/8500/9500 shared 4 MB "Boot TNT 0.1" ROM (the same
// image also serves the unemulated 7200).  Two revisions exist, differing
// only in the HWInit and Mac68KROM components.
static const char *const TNT[] = {"pm7500", "pm8500", "pm9500", NULL};
// Apple Network Server 500/700 ROMs.  TRAP: they share header sums with each
// other (three are $962F6C13) and with the Power Macintosh 9500 v2 ROM
// (2.26B6 is $9630C68B), and the production image carries the same version
// string as the 9500 v2 ROM.  Only the full id — header sum plus the
// ConfigInfo 64-bit sum — tells them apart.  Never identify one by version.
static const char *const ANS[] = {"ans500", "ans700", NULL};
// Apple Lisa 2 (rev H) and Macintosh XL ("3A") interleaved boot ROMs
// (docs/machines/lisa/lisa.md §16), each two 8 KB byte-slice chips.
static const char *const LISA[] = {"lisa", NULL};
static const char *const MACXL[] = {"macxl", NULL};

// === The table ============================================================
//
// {family_name, compatible, id, rom_size, checksum_span, flags}
// Within each kind: rows verified from images first, then rows from the
// published list, then rows known from checksum values alone.

const rom_info_t rom_table[] = {
    // --- Mac 68k: the header sum at offset 0 ---
    // verified from images
    {"Macintosh-512k ROM",                                                  NONE,      "28ba4e50",                  64 * 1024,   0,          0             },
    {"Macintosh-128k ROM",                                                  NONE,      "28ba61ce",                  64 * 1024,   0,          0             },
    {"Macintosh Plus (Rev 2, Lonely Heifers)",                              PLUS,      "4d1eeae1",                  128 * 1024,  0,          0             },
    {"Macintosh Plus (Rev 1, Lonely Hearts)",                               PLUS,      "4d1eeee1",                  128 * 1024,  0,          0             },
    {"Macintosh Plus (Rev 3, Loud Harmonicas)",                             PLUS,      "4d1f8172",                  128 * 1024,  0,          0             },
    {"PowerBook 100 ROM",                                                   NONE,      "96645f9c",                  256 * 1024,  0,          0             },
    {"Mac Portable ROM",                                                    NONE,      "96ca3846",                  256 * 1024,  0,          0             },
    {"Universal IIx/IIcx/SE/30 ROM",                                        UNIVERSAL, "97221136",                  256 * 1024,  0,          0             },
    {"MacII (800k v2) ROM",                                                 NONE,      "9779d2c4",                  256 * 1024,  0,          0             },
    {"MacII (800k v1) ROM",                                                 NONE,      "97851db6",                  256 * 1024,  0,          0             },
    {"Mac SE ROM",                                                          NONE,      "b2e362a8",                  256 * 1024,  0,          0             },
    {"Mac SE FDHD ROM",                                                     NONE,      "b306e171",                  256 * 1024,  0,          0             },
    {"Classic II ROM",                                                      NONE,      "3193670e",                  512 * 1024,  0,          0             },
    {"Mac LC ROM",                                                          NONE,      "350eacf0",                  512 * 1024,  0,          0             },
    {"Mac LCII ROM",                                                        NONE,      "35c28f5f",                  512 * 1024,  0,          0             },
    {"Macintosh IIci ROM",                                                  IICI,      "368cadfe",                  512 * 1024,  0,          0             },
    {"Macintosh IIsi ROM",                                                  IISI,      "36b7fb6c",                  512 * 1024,  0,          0             },
    {"Macintosh IIfx ROM",                                                  IIFX,      "4147dd77",                  512 * 1024,  0,          0             },
    // The Classic's header sum stops at 256 KB, where its built-in ROM disk starts.
    {"Macintosh Classic ROM",                                               NONE,      "a49f9914",                  512 * 1024,  256 * 1024, 0             },
    {"Powerbook Duo 270 ROM",                                               NONE,      "0024d346",                  1024 * 1024, 0,          0             },
    {"Powerbook 280&280c ROM",                                              NONE,      "015621d7",                  1024 * 1024, 0,          0             },
    {"Performa 580 & 588 ROM",                                              NONE,      "064dc91d",                  1024 * 1024, 0,          0             },
    {"Quadra 630 ROM",                                                      NONE,      "06684214",                  1024 * 1024, 0,          0             },
    {"Quadra 950 ROM",                                                      Q950,      "3dc27823",                  1024 * 1024, 0,          0             },
    {"Quadra 700/900 ROM",                                                  Q700_Q900, "420dbff3",                  1024 * 1024, 0,          0             },
    {"MacIIvx & IIvi ROM",                                                  NONE,      "4957eb49",                  1024 * 1024, 0,          0             },
    {"Powerbook 160 & 165c & 180 & 180c ROM",                               NONE,      "e33b2724",                  1024 * 1024, 0,          0             },
    {"LCIII (older) ROM",                                                   NONE,      "ec904829",                  1024 * 1024, 0,          0             },
    {"Mac LCIII ROM",                                                       NONE,      "ecbbc41c",                  1024 * 1024, 0,          0             },
    {"Color Classic ROM",                                                   NONE,      "ecd99dc0",                  1024 * 1024, 0,          0             },
    {"Powerbook 210,230,250 ROM",                                           NONE,      "ecfa989b",                  1024 * 1024, 0,          0             },
    {"Color Classic II & LC 550 & Performa 275,550,560 & Macintosh TV ROM", NONE,      "ede66cbd",                  1024 * 1024, 0,          0             },
    {"Centris 610,650 ROM",                                                 NONE,      "f1a6f343",                  1024 * 1024, 0,          0             },
    {"Quadra 610,650,maybe 800 ROM",                                        NONE,      "f1acad13",                  1024 * 1024, 0,          0             },
    {"Powerbook 150 ROM",                                                   NONE,      "fda22562",                  1024 * 1024, 0,          0             },
    {"Quadra 605 ROM",                                                      NONE,      "ff7439ee",                  1024 * 1024, 0,          0             },
    {"Powerbook 190cs ROM",                                                 NONE,      "4d27039c",                  2048 * 1024, 0,          0             },
    {"Quadra 840AV/660AV ROM",                                              AV,        "5bf10fd1",                  2048 * 1024, 0,          0             },
    {"PowerBook 520&520c&540&540c ROM",                                     NONE,      "b6909089",                  2048 * 1024, 0,          0             },
    // from the published checksum list
    {"Unitron Mac 512 (clone) ROM",                                         NONE,      "2882f5fa",                  64 * 1024,   0,          0             },
    {"Macintosh prototype (Twiggy) ROM",                                    NONE,      "2884371d",                  64 * 1024,   0,          0             },
    {"PowerBook 550c (Japan) ROM",                                          NONE,      "b57687a5",                  2048 * 1024, 0,          0             },
    // --- Old World PowerPC (4 MiB): header sum and ConfigInfo 64-bit sum ---
    // verified from images
    // Bandai Pippin: the ConfigInfo sums cover a range other than the whole image.
    {"Bandai Pippin (Kinka 1.0) ROM",                                       NONE,      "2bef21b7-0bf68e274dc0720d", 4096 * 1024, 0,          ROM_F_NO_SUM64},
    {"Bandai Pippin (Kinka Dev) ROM",                                       NONE,      "2bf65931-2b807a9748e78318", 4096 * 1024, 0,          ROM_F_NO_SUM64},
    {"Apple Network Server 500/700 ROM (2.0 prototype, Mac OS)",            ANS,       "49b2be8f-5f2aeeb25507b2cb", 4096 * 1024, 0,          0             },
    {"Power Mac & Performa 5200,5300,6200,6300 ROM",                        NONE,      "63abfd3f-c5421fbaff3c5a9d", 4096 * 1024, 0,          0             },
    {"Power Mac 6500 ROM",                                                  NONE,      "6e92fe08-c784f8035da2d93a", 4096 * 1024, 0,          0             },
    {"Performa 6400 ROM",                                                   NONE,      "6f5724c0-6703442013f443d8", 4096 * 1024, 0,          0             },
    {"Power Mac G3 (v3) ROM",                                               NONE,      "78f57389-7b8375af2e19914d", 4096 * 1024, 0,          0             },
    {"Power Mac G3 desktop ROM",                                            NONE,      "79d68d63-32d284c61fd4f342", 4096 * 1024, 0,          0             },
    {"PowerBook 1400cs ROM",                                                NONE,      "83a21950-b470fc5d287e39c1", 4096 * 1024, 0,          0             },
    {"Powerbook 2300 & PB5x0 PPC Upgrade ROM",                              NONE,      "83c54f75-c8b9658674ebb5ba", 4096 * 1024, 0,          0             },
    {"Power Mac 7300 & 7600 & 8600 & 9600 (v1) ROM",                        NONE,      "960e4be9-949c2c56d07516b7", 4096 * 1024, 0,          0             },
    {"Power Mac 8600 & 9600 (v2) ROM",                                      NONE,      "960fc647-013bb98cd4ad16c4", 4096 * 1024, 0,          0             },
    {"Apple Network Server 500/700 ROM (Open Firmware 2.26NT, Windows NT)", ANS,       "962f6c13-50348b3d0126096b",
     4096 * 1024,                                                                                                                0,          0             },
    {"Apple Network Server 500/700 ROM (Open Firmware 1.1.20.1)",           ANS,       "962f6c13-c60da96de537f08a", 4096 * 1024, 0,          0             },
    {"Apple Network Server 500/700 ROM (Open Firmware 1.1.22)",             ANS,       "962f6c13-d540b3dd5bcf9caa", 4096 * 1024, 0,          0             },
    {"Power Macintosh 7500/8500/9500 ROM (v2)",                             TNT,       "9630c68b-4db4a42fea3b53b3", 4096 * 1024, 0,          0             },
    {"Apple Network Server 500/700 ROM (Open Firmware 2.26B6)",             ANS,       "9630c68b-a71fb907dd180b8a", 4096 * 1024, 0,          0             },
    {"Power Macintosh 7500/8500/9500 ROM (v1)",                             TNT,       "96cd923d-c241cd82bf90797a", 4096 * 1024, 0,          0             },
    {"Workgroup Server 9150-120 ROM",                                       NONE,      "9b037f6f-d20052bd25d88bd6", 4096 * 1024, 0,          0             },
    {"Power Macintosh 7100 ROM (newer, Boot PDM 601 1.1)",                  PM7100,    "9b7a3aad-ed510ac937721e25", 4096 * 1024, 0,          0             },
    {"Workgroup Server 9150-80 ROM",                                        NONE,      "9c7c98f7-e9220fe5992ffdf2", 4096 * 1024, 0,          0             },
    {"Power Macintosh 6100/7100/8100 ROM",                                  PDM,       "9feb69b3-dedc602cfc3b9221", 4096 * 1024, 0,          0             },
    {"PowerBook G3 Wallstreet PDQ ROM",                                     NONE,      "b46ffb63-69e8e6201908f35f", 4096 * 1024, 0,          0             },
    {"PowerBook G3 Wallstreet ROM",                                         NONE,      "cbb01212-833504c219032ad5", 4096 * 1024, 0,          0             },
    // from the published checksum list
    {"PowerBook G3 (Kanga) ROM",                                            NONE,      "2560f229-afabd023ad5fc165", 4096 * 1024, 0,          0             },
    {"PowerBook 3400c ROM",                                                 NONE,      "276ec1f1-05f6c10c61a3760d", 4096 * 1024, 0,          0             },
    {"Bandai Pippin (Kinka 1.2) ROM",                                       NONE,      "3e10e14c-dced07b8dfabe232", 4096 * 1024, 0,          ROM_F_NO_SUM64},
    {"Bandai Pippin (Kinka 1.3) ROM",                                       NONE,      "3e6b3ee4-f1c39bbb1627982b", 4096 * 1024, 0,          ROM_F_NO_SUM64},
    {"CHRP Mac OS ROM 3.3b4",                                               NONE,      "4266511f-03134cffc7c9fc7e", 4096 * 1024, 0,          0             },
    {"Power Express (TriPEx) ROM",                                          NONE,      "4604518f-efbc0e0922ebe984", 4096 * 1024, 0,          0             },
    {"Power Mac 4400, Motorola StarMax 4400/7220 ROM",                      NONE,      "58f03416-5208e606711dd704", 4096 * 1024, 0,          0             },
    {"TNT development ROM (A5c1)",                                          NONE,      "67a1aa96-48877909cdce7677", 4096 * 1024, 0,          0             },
    {"Power Mac G3 (Beige), rev D ROM",                                     NONE,      "78e842a8-7ce12c8aef3312fe", 4096 * 1024, 0,          0             },
    {"Power Mac G3 (Beige), rev B ROM",                                     NONE,      "78fdb784-e8cd35c33d1c0b0b", 4096 * 1024, 0,          0             },
    {"PowerBook 1400 ROM",                                                  NONE,      "838c0831-b99cdb0cd1d6e068", 4096 * 1024, 0,          0             },
    // from published checksum values only
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "3b6531e1-e18f29e5880b3862", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "3b712c0f-80b7b413a86fb9f6", 4096 * 1024, 0,          0             },
    {"Old World Power Macintosh ROM (no dump held)",                        NONE,      "852cfbdf-811cb90fcd4cac65", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "b8b2c971-3c28e7cb0cdf7247", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "b8bea8b3-1f3fed032df9277a", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "b8c832f3-68991dd330bdbf29", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "b8d0b672-8918fbc074ab83d4", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "b9eb8c3d-37ff1fc591c27899", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "c753c667-5716eaf58bd3dda1", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "c75c6aab-d9d8584aebaed648", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "c7cb0323-fd4d237a53b399ad", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "c804f7f4-c456af44bf15c59a", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "c8e1be97-d666e7791a0313b8", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "c90b6289-a63fc25ec62408b0", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "c92f71d3-f71fd99f08063d40", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "cdb5d438-c75bbac00ffa2957", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "cde9cda4-e672c2885b5af6d0", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "ce144c14-aa0a81552ece4ed0", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "ce1b9fd2-613a847c5a93f1f7", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "ce1cf7f7-6a8d3345ced59eef", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "ce1fd217-6e49bdb3f55e4583", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "ce2a2a5b-64278ec15492fff5", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "ce8a3b5c-f0d9214daef11693", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "d36ba902-5cba340b0eeb2a67", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "d377adb7-1051c5a4583a46b2", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "e20aa0d0-19846d88d890eafe", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "e561e1f6-fc1c89dafc8510fd", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "ea00f1b7-794eda200c4ee2c9", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "eabc97ef-ead67f526c472d8b", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "eacb3ca4-8e6380cf499094b3", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "ec849611-7840123158b8943c", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "ec86128e-380ad7bdcaa89dc7", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "ec93ab73-995426703e59abff", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "ec96aeb6-5c3097a900eebbe5", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "ecaf0460-22cde263f891fa1b", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "ecb73ad5-e8bf5f2e952e87c0", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "ecb7c4f9-0e6d36f70dba2607", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "ecb8e951-618a1da98364bf3f", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "ecb96443-1de4e772a4c70d9e", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "ecbd9bd2-ec3ca440cd55c910", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "ecc44a65-bed3a4b747213d92", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "ecc53d80-2714526e8d671114", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "ecc53d80-3b686b2094293982", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "ecc53d80-5d961b568bf847ea", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "ecc6f29a-abd8266a6d349401", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "ecce15c2-e6b8f27f97a78911", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "ecd3453f-cdf9eb8fe542da0e", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "ecef6af1-cf149c7394b8c437", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "ed26a1ef-a2a603b52bf6017b", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "ed7f9fc2-27ab69c9f21df9d1", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "ee6bc7d9-71ea5cd85625c3a2", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "eece7cd0-bf44006b576a9770", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "eed28047-ac8a8f4cec6d8d3b", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "fcaad843-9525d4d735b28e12", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "fd12b69e-d3df605d1d1c72c3", 4096 * 1024, 0,          0             },
    {"Mac OS ROM file (New World System Folder image)",                     NONE,      "fd86d120-465bf79ac11efadb", 4096 * 1024, 0,          0             },
    // --- Lisa / Macintosh XL: the check word at $3FFE ---
    // verified from images
    {"Apple Lisa 2 Boot ROM (rev H)",                                       LISA,      "3f7b",                      16 * 1024,   0,          0             },
    {"Macintosh XL Boot ROM (\"3A\")",                                      MACXL,     "d905",                      16 * 1024,   0,          0             },
};

const size_t rom_table_count = sizeof(rom_table) / sizeof(rom_table[0]);
