// The unit tier's platform header: the shared one (src/platform/platform.h),
// with a clock that stands still (platform_clock.h beside this file), so a
// test's timing never depends on the host.  Force-included by every suite and
// found by core headers' #include "platform.h".
#ifndef UNIT_PLATFORM_H
#define UNIT_PLATFORM_H
#include "../../../src/platform/platform.h"
#endif // UNIT_PLATFORM_H
