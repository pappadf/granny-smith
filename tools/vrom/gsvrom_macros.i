; SPDX-License-Identifier: MIT
; Copyright (c) pappadf
;
; gsvrom_macros.i
; Structural macros for the GS generic declaration ROM (vasm mot syntax).
; Ports of the MPW ROMEqu.a idioms: an sResource list entry is one long
; whose top byte is the entry ID; OSLstEntry packs a signed self-relative
; 24-bit offset in the low bits, DatLstEntry packs immediate data.
; (MPW's `++` / `**` operators are OR / AND — see proposal sec. 8.)

; OSLstEntry <id>,<label> — offset-form list entry pointing at <label>.
OSLstEntry      macro
                dc.l    ((\1)<<24)|(((\2)-*)&$00FFFFFF)
                endm

; DatLstEntry <id>,<value> — data-form list entry (16-bit immediate).
DatLstEntry     macro
                dc.l    ((\1)<<24)|((\2)&$0000FFFF)
                endm

; EndOfList — the $FF00_0000 sentinel that terminates every list.
EndOfList       macro
                dc.l    $FF000000
                endm

; sBlockBegin/End <label> — a length-prefixed sBlock; the long size field
; includes itself.  Usage:
;       sBlockBegin MyBlock
;       ...body...
;       sBlockEnd   MyBlock
sBlockBegin     macro
\1              dc.l    \1_end-\1
                endm
sBlockEnd       macro
\1_end          equ     *
                endm

; sExecBegin/End <label>,<cpu> — an SExecBlock: long physical size, then
; revision $02, CPU id byte, reserved word, and a long self-relative
; offset from the offset field to the entry point.  <cpu> is 1..4 for
; 68000..68040.  (Layout confirmed against the 24AC ROM's PrimaryInit
; header 00 00 09 BE 02 02 00 00 00 00 00 04 — offset 4 → code follows.)
sExecBegin      macro
\1              dc.l    \1_end-\1       ; physical block size (incl. self)
                dc.b    2               ; sExec revision
                dc.b    \2              ; CPU id (1=68000 .. 4=68040)
                dc.w    0               ; reserved
                dc.l    \1_code-*       ; self-relative offset to entry point
\1_code         equ     *
                endm
sExecEnd        macro
\1_end          equ     *
                endm

; cString <label>,<text> — NUL-terminated C string, word-aligned after.
cString         macro
\1              dc.b    \2,0
                even
                endm

; VPBlk <label>,<width>,<height>,<bpp> — an mVidParams sBlock: a PixMap
; record (minus runtime fields) the Slot Manager copies into the
; gDevice's pmHandle (nubus_vrom.md sec. 6.2).  Indexed chunky modes.
VPBlk           macro
\1:             dc.l    \1_e-\1         ; physical block size
                dc.l    0               ; vpBaseOffset (page 0 at FB base)
                dc.w    (\2)*(\4)/8     ; vpRowBytes (tight stride)
                dc.w    0,0,\3,\2       ; vpBounds t/l/b/r
                dc.w    1               ; vpVersion
                dc.w    0               ; vpPackType
                dc.l    0               ; vpPackSize
                dc.l    $00480000       ; vpHRes: 72 dpi Fixed
                dc.l    $00480000       ; vpVRes
                dc.w    0               ; vpPixelType: chunky indexed
                dc.w    \4              ; vpPixelSize
                dc.w    1               ; vpCmpCount
                dc.w    \4              ; vpCmpSize
                dc.l    0               ; vpPlaneBytes
\1_e:
                endm

; VPBlkR <label>,<width>,<height>,<bpp>,<rowbytes> — indexed-chunky VPBlock
; with an EXPLICIT row pitch (the GC keeps 1024 bytes/row at every indexed
; depth regardless of width×bpp).
VPBlkR          macro
\1:             dc.l    \1_e-\1
                dc.l    0               ; vpBaseOffset
                dc.w    \5              ; vpRowBytes (fixed pitch)
                dc.w    0,0,\3,\2       ; vpBounds
                dc.w    1               ; vpVersion
                dc.w    0               ; vpPackType
                dc.l    0               ; vpPackSize
                dc.l    $00480000       ; vpHRes
                dc.l    $00480000       ; vpVRes
                dc.w    0               ; vpPixelType: chunky indexed
                dc.w    \4              ; vpPixelSize
                dc.w    1               ; vpCmpCount
                dc.w    \4              ; vpCmpSize
                dc.l    0               ; vpPlaneBytes
\1_e:
                endm

; VPBlkO <label>,<width>,<height>,<bpp>,<rowbytes>,<baseoff> — indexed
; VPBlock with explicit row pitch AND vpBaseOffset (the GC's framebuffer
; sits $11400 into the device window; the OS adds vpBaseOffset to the
; device base when it builds the PixMap).
VPBlkO          macro
\1:             dc.l    \1_e-\1
                dc.l    \6              ; vpBaseOffset
                dc.w    \5              ; vpRowBytes (fixed pitch)
                dc.w    0,0,\3,\2       ; vpBounds
                dc.w    1               ; vpVersion
                dc.w    0               ; vpPackType
                dc.l    0               ; vpPackSize
                dc.l    $00480000       ; vpHRes
                dc.l    $00480000       ; vpVRes
                dc.w    0               ; vpPixelType: chunky indexed
                dc.w    \4              ; vpPixelSize
                dc.w    1               ; vpCmpCount
                dc.w    \4              ; vpCmpSize
                dc.l    0               ; vpPlaneBytes
\1_e:
                endm

; VPBlkD <label>,<width>,<height>,<bpp>,<cmpsize> — direct-RGB variant of
; VPBlk: ChunkyDirect pixel type, 3 components of <cmpsize> bits.
VPBlkD          macro
\1:             dc.l    \1_e-\1
                dc.l    0               ; vpBaseOffset
                dc.w    (\2)*(\4)/8     ; vpRowBytes
                dc.w    0,0,\3,\2       ; vpBounds
                dc.w    1               ; vpVersion
                dc.w    0               ; vpPackType
                dc.l    0               ; vpPackSize
                dc.l    $00480000       ; vpHRes
                dc.l    $00480000       ; vpVRes
                dc.w    $10             ; vpPixelType: chunky direct
                dc.w    \4              ; vpPixelSize
                dc.w    3               ; vpCmpCount
                dc.w    \5              ; vpCmpSize (5 for 16 bpp, 8 for 32)
                dc.l    0               ; vpPlaneBytes
\1_e:
                endm

; VideoSRsrc4 <id-suffix>,<width>,<height> — one functional video
; sResource with the four indexed depths (1/2/4/8 bpp) as mode-list
; entries $80..$83.  References the shared GSVidType/GSVidName/
; GSDrvrDir/GSMinorBase records emitted once by the top-level source.
VideoSRsrc4     macro
VS\1:           OSLstEntry sRsrcType,GSVidType
                OSLstEntry sRsrcName,GSVidName
                OSLstEntry sRsrcDrvrDir,GSDrvrDir
                DatLstEntry sRsrcFlags,fOpenAtStart
                DatLstEntry sRsrcHWDevId,1
                OSLstEntry MinorBaseOS,GSMinorBase
                OSLstEntry MinorLength,VS\1Len
                OSLstEntry FirstVidMode,VS\1M1
                OSLstEntry SecondVidMode,VS\1M2
                OSLstEntry ThirdVidMode,VS\1M3
                OSLstEntry FourthVidMode,VS\1M4
                EndOfList
VS\1Len:        dc.l    (\2)*(\3)       ; framebuffer bytes at max depth
VS\1M1:         OSLstEntry mVidParams,VS\1P1
                DatLstEntry mPageCnt,1
                DatLstEntry mDevType,clutType
                EndOfList
VS\1M2:         OSLstEntry mVidParams,VS\1P2
                DatLstEntry mPageCnt,1
                DatLstEntry mDevType,clutType
                EndOfList
VS\1M3:         OSLstEntry mVidParams,VS\1P3
                DatLstEntry mPageCnt,1
                DatLstEntry mDevType,clutType
                EndOfList
VS\1M4:         OSLstEntry mVidParams,VS\1P4
                DatLstEntry mPageCnt,1
                DatLstEntry mDevType,clutType
                EndOfList
                VPBlk   VS\1P1,\2,\3,1
                VPBlk   VS\1P2,\2,\3,2
                VPBlk   VS\1P3,\2,\3,4
                VPBlk   VS\1P4,\2,\3,8
                endm

; VideoSRsrc5D <id-suffix>,<width>,<height> — functional video sResource
; with the 24AC's five depths: indexed 1/4/8 bpp ($80..$82) plus direct
; 16/32 bpp ($83/$84).
VideoSRsrc5D    macro
VS\1:           OSLstEntry sRsrcType,GSVidType
                OSLstEntry sRsrcName,GSVidName
                OSLstEntry sRsrcDrvrDir,GSDrvrDir
                DatLstEntry sRsrcFlags,fOpenAtStart
                DatLstEntry sRsrcHWDevId,1
                OSLstEntry MinorBaseOS,GSMinorBase
                OSLstEntry MinorLength,VS\1Len
                OSLstEntry FirstVidMode,VS\1M1
                OSLstEntry SecondVidMode,VS\1M2
                OSLstEntry ThirdVidMode,VS\1M3
                OSLstEntry FourthVidMode,VS\1M4
                OSLstEntry FifthVidMode,VS\1M5
                EndOfList
VS\1Len:        dc.l    (\2)*(\3)*4     ; framebuffer bytes at 32 bpp
VS\1M1:         OSLstEntry mVidParams,VS\1P1
                DatLstEntry mPageCnt,1
                DatLstEntry mDevType,clutType
                EndOfList
VS\1M2:         OSLstEntry mVidParams,VS\1P2
                DatLstEntry mPageCnt,1
                DatLstEntry mDevType,clutType
                EndOfList
VS\1M3:         OSLstEntry mVidParams,VS\1P3
                DatLstEntry mPageCnt,1
                DatLstEntry mDevType,clutType
                EndOfList
VS\1M4:         OSLstEntry mVidParams,VS\1P4
                DatLstEntry mPageCnt,1
                DatLstEntry mDevType,directType
                EndOfList
VS\1M5:         OSLstEntry mVidParams,VS\1P5
                DatLstEntry mPageCnt,1
                DatLstEntry mDevType,directType
                EndOfList
                VPBlk   VS\1P1,\2,\3,1
                VPBlk   VS\1P2,\2,\3,4
                VPBlk   VS\1P3,\2,\3,8
                VPBlkD  VS\1P4,\2,\3,16,5
                VPBlkD  VS\1P5,\2,\3,32,8
                endm
