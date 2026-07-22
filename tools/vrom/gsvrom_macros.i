| SPDX-License-Identifier: MIT
| Copyright (c) pappadf
|
| gsvrom_macros.i
| Structural macros for the GS generic declaration ROM (GNU as, .altmacro).
| Ports of the MPW ROMEqu.a idioms: an sResource list entry is one long
| whose top byte is the entry ID; OSLstEntry packs a signed self-relative
| 24-bit offset in the low bits, DatLstEntry packs immediate data.
|
| PORT NOTE (runtime-vrom proposal sec. 2.1, Appendix A): gas silently
| folds a masked forward-referenced difference OR'd with a constant to
| ZERO — `(id<<24)|((label-.)&0xFFFFFF)` emits a corrupt zero offset with
| no diagnostic.  The ID byte and the masked 24-bit offset occupy
| disjoint bits, so `|` is arithmetically replaceable by `+`, which
| assembles correctly for both forward and negative offsets.  Every
| packed entry below therefore uses `+`, never `|`.

| OSLstEntry <id>,<label> — offset-form list entry pointing at <label>.
	.macro	OSLstEntry id, label
	dc.l	((\id)<<24)+(((\label)-.)&0x00FFFFFF)
	.endm

| DatLstEntry <id>,<value> — data-form list entry (16-bit immediate).
	.macro	DatLstEntry id, value
	dc.l	((\id)<<24)+((\value)&0x0000FFFF)
	.endm

| EndOfList — the 0xFF000000 sentinel that terminates every list.
	.macro	EndOfList
	dc.l	0xFF000000
	.endm

| sBlockBegin/End <label> — a length-prefixed sBlock; the long size field
| includes itself.  Usage:
|       sBlockBegin MyBlock
|       ...body...
|       sBlockEnd   MyBlock
	.macro	sBlockBegin lab
\lab&:	dc.l	\lab&_end-\lab
	.endm
	.macro	sBlockEnd lab
\lab&_end:
	.endm

| sExecBegin/End <label>,<cpu> — an SExecBlock: long physical size, then
| revision 0x02, CPU id byte, reserved word, and a long self-relative
| offset from the offset field to the entry point.  <cpu> is 1..4 for
| 68000..68040.  (Layout confirmed against the 24AC ROM's PrimaryInit
| header 00 00 09 BE 02 02 00 00 00 00 00 04 — offset 4 -> code follows.)
	.macro	sExecBegin lab, cpu
\lab&:	dc.l	\lab&_end-\lab          | physical block size (incl. self)
	dc.b	2                       | sExec revision
	dc.b	\cpu                    | CPU id (1=68000 .. 4=68040)
	dc.w	0                       | reserved
	dc.l	\lab&_code-.            | self-relative offset to entry point
\lab&_code:
	.endm
	.macro	sExecEnd lab
\lab&_end:
	.endm

| (vasm's cString macro is gone: gas's dc.b SILENTLY DROPS string
| operands, so NUL-terminated strings are written inline as
|       Label:  .asciz "text"
|               .balign 2
| at each call site instead.)

| VPBlk <label>,<width>,<height>,<bpp> — an mVidParams sBlock: a PixMap
| record (minus runtime fields) the Slot Manager copies into the
| gDevice's pmHandle (nubus_vrom.md sec. 6.2).  Indexed chunky modes.
	.macro	VPBlk lab, w, h, bpp
\lab&:	dc.l	\lab&_e-\lab            | physical block size
	dc.l	0                       | vpBaseOffset (page 0 at FB base)
	dc.w	(\w)*(\bpp)/8           | vpRowBytes (tight stride)
	dc.w	0,0,\h,\w               | vpBounds t/l/b/r
	dc.w	1                       | vpVersion
	dc.w	0                       | vpPackType
	dc.l	0                       | vpPackSize
	dc.l	0x00480000              | vpHRes: 72 dpi Fixed
	dc.l	0x00480000              | vpVRes
	dc.w	0                       | vpPixelType: chunky indexed
	dc.w	\bpp                    | vpPixelSize
	dc.w	1                       | vpCmpCount
	dc.w	\bpp                    | vpCmpSize
	dc.l	0                       | vpPlaneBytes
\lab&_e:
	.endm

| VPBlkR <label>,<width>,<height>,<bpp>,<rowbytes> — indexed-chunky VPBlock
| with an EXPLICIT row pitch (the GC keeps 1024 bytes/row at every indexed
| depth regardless of width*bpp).
	.macro	VPBlkR lab, w, h, bpp, rb
\lab&:	dc.l	\lab&_e-\lab
	dc.l	0                       | vpBaseOffset
	dc.w	\rb                     | vpRowBytes (fixed pitch)
	dc.w	0,0,\h,\w               | vpBounds
	dc.w	1                       | vpVersion
	dc.w	0                       | vpPackType
	dc.l	0                       | vpPackSize
	dc.l	0x00480000              | vpHRes
	dc.l	0x00480000              | vpVRes
	dc.w	0                       | vpPixelType: chunky indexed
	dc.w	\bpp                    | vpPixelSize
	dc.w	1                       | vpCmpCount
	dc.w	\bpp                    | vpCmpSize
	dc.l	0                       | vpPlaneBytes
\lab&_e:
	.endm

| VPBlkO <label>,<width>,<height>,<bpp>,<rowbytes>,<baseoff> — indexed
| VPBlock with explicit row pitch AND vpBaseOffset (the GC's framebuffer
| sits 0x11400 into the device window; the OS adds vpBaseOffset to the
| device base when it builds the PixMap).
	.macro	VPBlkO lab, w, h, bpp, rb, baseoff
\lab&:	dc.l	\lab&_e-\lab
	dc.l	\baseoff                | vpBaseOffset
	dc.w	\rb                     | vpRowBytes (fixed pitch)
	dc.w	0,0,\h,\w               | vpBounds
	dc.w	1                       | vpVersion
	dc.w	0                       | vpPackType
	dc.l	0                       | vpPackSize
	dc.l	0x00480000              | vpHRes
	dc.l	0x00480000              | vpVRes
	dc.w	0                       | vpPixelType: chunky indexed
	dc.w	\bpp                    | vpPixelSize
	dc.w	1                       | vpCmpCount
	dc.w	\bpp                    | vpCmpSize
	dc.l	0                       | vpPlaneBytes
\lab&_e:
	.endm

| VPBlkD <label>,<width>,<height>,<bpp>,<cmpsize> — direct-RGB variant of
| VPBlk: ChunkyDirect pixel type, 3 components of <cmpsize> bits.
	.macro	VPBlkD lab, w, h, bpp, cmpsz
\lab&:	dc.l	\lab&_e-\lab
	dc.l	0                       | vpBaseOffset
	dc.w	(\w)*(\bpp)/8           | vpRowBytes
	dc.w	0,0,\h,\w               | vpBounds
	dc.w	1                       | vpVersion
	dc.w	0                       | vpPackType
	dc.l	0                       | vpPackSize
	dc.l	0x00480000              | vpHRes
	dc.l	0x00480000              | vpVRes
	dc.w	0x10                    | vpPixelType: chunky direct
	dc.w	\bpp                    | vpPixelSize
	dc.w	3                       | vpCmpCount
	dc.w	\cmpsz                  | vpCmpSize (5 for 16 bpp, 8 for 32)
	dc.l	0                       | vpPlaneBytes
\lab&_e:
	.endm

| VideoSRsrc4 <id-suffix>,<width>,<height> — one functional video
| sResource with the four indexed depths (1/2/4/8 bpp) as mode-list
| entries 0x80..0x83.  References the shared GSVidType/GSVidName/
| GSDrvrDir/GSMinorBase records emitted once by the top-level source.
	.macro	VideoSRsrc4 sfx, w, h
VS\sfx&:
	OSLstEntry sRsrcType,GSVidType
	OSLstEntry sRsrcName,GSVidName
	OSLstEntry sRsrcDrvrDir,GSDrvrDir
	DatLstEntry sRsrcFlags,fOpenAtStart
	DatLstEntry sRsrcHWDevId,1
	OSLstEntry MinorBaseOS,GSMinorBase
	OSLstEntry MinorLength,VS\sfx&Len
	OSLstEntry FirstVidMode,VS\sfx&M1
	OSLstEntry SecondVidMode,VS\sfx&M2
	OSLstEntry ThirdVidMode,VS\sfx&M3
	OSLstEntry FourthVidMode,VS\sfx&M4
	EndOfList
VS\sfx&Len:
	dc.l	(\w)*(\h)               | framebuffer bytes at max depth
VS\sfx&M1:
	OSLstEntry mVidParams,VS\sfx&P1
	DatLstEntry mPageCnt,1
	DatLstEntry mDevType,clutType
	EndOfList
VS\sfx&M2:
	OSLstEntry mVidParams,VS\sfx&P2
	DatLstEntry mPageCnt,1
	DatLstEntry mDevType,clutType
	EndOfList
VS\sfx&M3:
	OSLstEntry mVidParams,VS\sfx&P3
	DatLstEntry mPageCnt,1
	DatLstEntry mDevType,clutType
	EndOfList
VS\sfx&M4:
	OSLstEntry mVidParams,VS\sfx&P4
	DatLstEntry mPageCnt,1
	DatLstEntry mDevType,clutType
	EndOfList
	VPBlk	VS\sfx&P1,\w,\h,1
	VPBlk	VS\sfx&P2,\w,\h,2
	VPBlk	VS\sfx&P3,\w,\h,4
	VPBlk	VS\sfx&P4,\w,\h,8
	.endm

| VideoSRsrc5D <id-suffix>,<width>,<height> — functional video sResource
| with the 24AC's five depths: indexed 1/4/8 bpp (0x80..0x82) plus direct
| 16/32 bpp (0x83/0x84).
	.macro	VideoSRsrc5D sfx, w, h
VS\sfx&:
	OSLstEntry sRsrcType,GSVidType
	OSLstEntry sRsrcName,GSVidName
	OSLstEntry sRsrcDrvrDir,GSDrvrDir
	DatLstEntry sRsrcFlags,fOpenAtStart
	DatLstEntry sRsrcHWDevId,1
	OSLstEntry MinorBaseOS,GSMinorBase
	OSLstEntry MinorLength,VS\sfx&Len
	OSLstEntry FirstVidMode,VS\sfx&M1
	OSLstEntry SecondVidMode,VS\sfx&M2
	OSLstEntry ThirdVidMode,VS\sfx&M3
	OSLstEntry FourthVidMode,VS\sfx&M4
	OSLstEntry FifthVidMode,VS\sfx&M5
	EndOfList
VS\sfx&Len:
	dc.l	(\w)*(\h)*4             | framebuffer bytes at 32 bpp
VS\sfx&M1:
	OSLstEntry mVidParams,VS\sfx&P1
	DatLstEntry mPageCnt,1
	DatLstEntry mDevType,clutType
	EndOfList
VS\sfx&M2:
	OSLstEntry mVidParams,VS\sfx&P2
	DatLstEntry mPageCnt,1
	DatLstEntry mDevType,clutType
	EndOfList
VS\sfx&M3:
	OSLstEntry mVidParams,VS\sfx&P3
	DatLstEntry mPageCnt,1
	DatLstEntry mDevType,clutType
	EndOfList
VS\sfx&M4:
	OSLstEntry mVidParams,VS\sfx&P4
	DatLstEntry mPageCnt,1
	DatLstEntry mDevType,directType
	EndOfList
VS\sfx&M5:
	OSLstEntry mVidParams,VS\sfx&P5
	DatLstEntry mPageCnt,1
	DatLstEntry mDevType,directType
	EndOfList
	VPBlk	VS\sfx&P1,\w,\h,1
	VPBlk	VS\sfx&P2,\w,\h,4
	VPBlk	VS\sfx&P3,\w,\h,8
	VPBlkD	VS\sfx&P4,\w,\h,16,5
	VPBlkD	VS\sfx&P5,\w,\h,32,8
	.endm
