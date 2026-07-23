| SPDX-License-Identifier: MIT
| Copyright (c) pappadf
|
| frag_sinit.s
| Top-level for the SecondaryInit fragment (personalities with a
| deferred 32-bit sResource family — the GC).  Assembled to a raw
| self-contained sExecBlock (frag_<p>_sinit.bin).

	.altmacro
	.include "gsvrom_equ.i"
	.include "gsvrom_frag.i"

	.ifdef	PERSONALITY_MDCGC
	.include "ops_mdcgc.s"
	.endif

	.include "gsvrom_sinit.s"
