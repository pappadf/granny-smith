// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
// The scheduler suite's clock (see src/platform/platform.h): host_time_ms()
// is an extern the test implements over a controllable fake clock -- the pacing
// estimators (host_secs_per_vbl / host_secs_per_loop) are meaningless with the
// unit tier's constant-zero clock.  Found ahead of that one via -I$(CURDIR).
#ifndef PLATFORM_CLOCK_H
#define PLATFORM_CLOCK_H
double host_time_ms(void);
#endif // PLATFORM_CLOCK_H
