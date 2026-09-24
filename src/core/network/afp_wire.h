// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// afp_wire.h
// AFP wire constants: opcodes, the server and volume flag words, and the
// result codes (AFP 2.x, Inside AppleTalk ch. 13 and AFP_21_22).  ASP
// carries a command's result in the reply's ATP user bytes, and answers a
// command it cannot deliver with one of these -- which it used to spell as
// bare hex (0xFFFFEC62, 0xFFFFEC6A).

#ifndef AFP_WIRE_H
#define AFP_WIRE_H

// AFP opcodes
#define AFP_ByteRangeLock   0x01
#define AFP_CloseVol        0x02
#define AFP_CloseDir        0x03
#define AFP_CloseFork       0x04
#define AFP_CopyFile        0x05
#define AFP_CreateDir       0x06
#define AFP_CreateFile      0x07
#define AFP_Delete          0x08
#define AFP_Enumerate       0x09
#define AFP_Flush           0x0A
#define AFP_FlushFork       0x0B
#define AFP_GetForkParms    0x0E
#define AFP_GetSrvrInfo     0x0F
#define AFP_GetSrvrParms    0x10
#define AFP_GetVolParms     0x11
#define AFP_Login           0x12
#define AFP_LoginCont       0x13
#define AFP_Logout          0x14
#define AFP_MapID           0x15
#define AFP_MapName         0x16
#define AFP_MoveAndRename   0x17
#define AFP_OpenVol         0x18
#define AFP_OpenDir         0x19
#define AFP_OpenFork        0x1A
#define AFP_Read            0x1B
#define AFP_Rename          0x1C
#define AFP_SetDirParms     0x1D
#define AFP_SetFileParms    0x1E
#define AFP_SetForkParms    0x1F
#define AFP_SetVolParms     0x20
#define AFP_Write           0x21
#define AFP_GetFileDirParms 0x22
#define AFP_SetFileDirParms 0x23
#define AFP_ChangePassword  0x24
#define AFP_GetUserInfo     0x25
// AFP 2.1 additions (AFP_21_22 single-page.md line ~638)
#define AFP_GetSrvrMsg    0x26
#define AFP_CreateID      0x27
#define AFP_DeleteID      0x28
#define AFP_ResolveID     0x29
#define AFP_ExchangeFiles 0x2A
#define AFP_CatSearch     0x2B
#define AFP_OpenDT        0x30
#define AFP_CloseDT       0x31
#define AFP_GetIcon       0x33
#define AFP_GetIconInfo   0x34
#define AFP_AddAPPL       0x35
#define AFP_RmvAPPL       0x36
#define AFP_GetAPPL       0x37
#define AFP_AddComment    0x38
#define AFP_RmvComment    0x39
#define AFP_GetComment    0x3A
#define AFP_AddIcon       0xC0

// FPGetSrvrInfo / GetStatus Flags word (AFP_21_22 Table 1-4 + Figure 1-4).
#define AFP_SRVR_FLAG_COPYFILE       (1u << 0)
#define AFP_SRVR_FLAG_CHGPWD         (1u << 1)
#define AFP_SRVR_FLAG_NOSAVEPWD      (1u << 2)
#define AFP_SRVR_FLAG_SERVERMESSAGES (1u << 3)

// Volume Attributes word (AFP_21_22 Table 1-5 + Figure 1-5).
#define AFP_VOL_ATTR_READONLY    (1u << 0)
#define AFP_VOL_ATTR_HASPASSWORD (1u << 1)
#define AFP_VOL_ATTR_FILEIDS     (1u << 2)
#define AFP_VOL_ATTR_CATSEARCH   (1u << 3)

// AFP result codes (32-bit signed, two's complement)
#define AFPERR_NoErr            0x00000000u // 0
#define AFPERR_AccessDenied     0xFFFFEC78u // -5000
#define AFPERR_AuthContinue     0xFFFFEC77u // -5001
#define AFPERR_BadUAM           0xFFFFEC76u // -5002
#define AFPERR_BadVersNum       0xFFFFEC75u // -5003
#define AFPERR_BitmapErr        0xFFFFEC74u // -5004
#define AFPERR_CantMove         0xFFFFEC73u // -5005
#define AFPERR_DenyConflict     0xFFFFEC72u // -5006
#define AFPERR_DirNotEmpty      0xFFFFEC71u // -5007
#define AFPERR_DiskFull         0xFFFFEC70u // -5008
#define AFPERR_EOFErr           0xFFFFEC6Fu // -5009
#define AFPERR_FileBusy         0xFFFFEC6Eu // -5010
#define AFPERR_FlatVol          0xFFFFEC6Du // -5011
#define AFPERR_ItemNotFound     0xFFFFEC6Cu // -5012
#define AFPERR_LockErr          0xFFFFEC6Bu // -5013
#define AFPERR_MiscErr          0xFFFFEC6Au // -5014
#define AFPERR_NoMoreLocks      0xFFFFEC69u // -5015
#define AFPERR_NoServer         0xFFFFEC68u // -5016
#define AFPERR_ObjectExists     0xFFFFEC67u // -5017
#define AFPERR_ObjectNotFound   0xFFFFEC66u // -5018
#define AFPERR_ParamErr         0xFFFFEC65u // -5019
#define AFPERR_RangeNotLocked   0xFFFFEC64u // -5020
#define AFPERR_RangeOverlap     0xFFFFEC63u // -5021
#define AFPERR_SessClosed       0xFFFFEC62u // -5022
#define AFPERR_UserNotAuth      0xFFFFEC61u // -5023
#define AFPERR_CallNotSupported 0xFFFFEC60u // -5024
#define AFPERR_ObjectTypeErr    0xFFFFEC5Fu // -5025
#define AFPERR_TooManyFilesOpen 0xFFFFEC5Eu // -5026
#define AFPERR_ServerGoingDown  0xFFFFEC5Du // -5027
#define AFPERR_CantRename       0xFFFFEC5Cu // -5028
#define AFPERR_DirNotFound      0xFFFFEC5Bu // -5029
#define AFPERR_IconTypeError    0xFFFFEC5Au // -5030
#define AFPERR_VolLocked        0xFFFFEC59u // -5031
#define AFPERR_ObjectLocked     0xFFFFEC58u // -5032

// AFP 2.1 result codes (SysErr.a lines 606-648; AFP_21_22 p. 60)
#define AFPERR_ContainsSharedErr 0xFFFFEC57u // -5033
#define AFPERR_IDNotFound        0xFFFFEC56u // -5034
#define AFPERR_IDExists          0xFFFFEC55u // -5035
#define AFPERR_DiffVolErr        0xFFFFEC54u // -5036
#define AFPERR_CatalogChanged    0xFFFFEC53u // -5037
#define AFPERR_SameObjectErr     0xFFFFEC52u // -5038
#define AFPERR_BadIDErr          0xFFFFEC51u // -5039

#endif // AFP_WIRE_H
