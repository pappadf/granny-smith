// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// afp_wire.h
// AFP wire constants shared by the server and the transport under it: the
// result codes (AFP 2.x, Inside AppleTalk ch. 13 and AFP_21_22 p. 60).  ASP
// carries a command's result in the reply's ATP user bytes, and answers a
// command it cannot deliver with one of these -- which it used to spell as
// bare hex (0xFFFFEC62, 0xFFFFEC6A).

#ifndef AFP_WIRE_H
#define AFP_WIRE_H

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
