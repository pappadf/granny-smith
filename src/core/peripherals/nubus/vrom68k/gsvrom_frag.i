| SPDX-License-Identifier: MIT
| Copyright (c) pappadf
|
| gsvrom_frag.i
| Block-framing macros for the GS declaration-ROM 68K fragments.  Only
| the length-prefixed block shapes live here — the declarative-record
| macros (OSLstEntry & co.) are gone: every sResource record is now
| generated at runtime by the declrom builder (declrom.c), and these
| fragments are pure self-contained code blocks it splices verbatim.

| sBlockBegin/End <label> — a length-prefixed sBlock; the long size field
| includes itself.
	.macro	sBlockBegin lab
\lab&:	dc.l	\lab&_end-\lab
	.endm
	.macro	sBlockEnd lab
\lab&_end:
	.endm

| sExecBegin/End <label>,<cpu> — an SExecBlock: long physical size, then
| revision 0x02, CPU id byte, reserved word, and a long self-relative
| offset from the offset field to the entry point.  <cpu> is 1..4 for
| 68000..68040.
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
