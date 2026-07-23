| SPDX-License-Identifier: MIT
| Copyright (c) pappadf
|
| frag_drvr.s
| Top-level for the DRVR fragment: one personality selected by
| --defsym PERSONALITY_*=1, assembled to a raw size-prefixed DRVR sBlock
| (frag_<p>_drvr.bin) that the declrom builder splices verbatim.

	.altmacro
	.include "gsvrom_equ.i"
	.include "gsvrom_frag.i"

	.ifdef	PERSONALITY_JMFB
	.include "ops_jmfb.s"
	.endif
	.ifdef	PERSONALITY_BOOGIE
	.include "ops_boogie.s"
	.endif
	.ifdef	PERSONALITY_MDCGC
	.include "ops_mdcgc.s"
	.endif
	.ifdef	PERSONALITY_SE30
	.include "ops_se30.s"
	.endif

	.include "gsvrom_drvr.s"
