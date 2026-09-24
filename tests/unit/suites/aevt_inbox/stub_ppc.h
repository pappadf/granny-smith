// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// stub_ppc.h -- what the stub PPC layer lets a test see (stub_ppc.c).

#ifndef STUB_PPC_H
#define STUB_PPC_H

#include "appletalk_ppc.h"

extern int stub_ppc_blocks; // blocks written: the endpoint's replies
extern const ppc_session_t *stub_ppc_last_block_session;

ppc_session_t *stub_ppc_session(void); // the one open session
void stub_ppc_reset(void);

#endif
