// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// gs_assert.h
// Project-wide assertions (GS_ASSERT / GS_ASSERTF) and the unimplemented-
// feature fault (GS_UNIMPLEMENTED).  The handlers live in debug/debug.c.
//
// Named gs_assert.h, not assert.h: src/core is on the include path, so a
// core/assert.h would shadow libc <assert.h> for every TU (and for libc's
// own internal includes).  The GS_ / gs_ prefixes are kept for the same
// reason -- assertion names are the high-collision case.
//
// The variadic macros use the GNU `, ##__VA_ARGS__` extension to swallow the
// trailing comma when no arguments follow the format; the project builds
// with GCC/Clang only (docs/guide/STYLE_GUIDE.md, "Compiler Extensions").

#ifndef GS_ASSERT_H
#define GS_ASSERT_H

#ifdef __cplusplus
extern "C" {
#endif

// Failure handlers print diagnostics (host + target backtraces, process info),
// then pause the scheduler and RETURN -- execution continues past the check.
// gs_assert_fail backs GS_ASSERT (no message, so no varargs marshalling at
// every plain assertion site); gs_assert_failf backs GS_ASSERTF.
void gs_assert_fail(const char *expr, const char *file, int line, const char *func);
void gs_assert_failf(const char *expr, const char *file, int line, const char *func, const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 5, 6)))
#endif
    ;

// Basic assert macros.  Enabled in every build except the GS_FAST production
// profile (wasm release / headless MODE=fast), where they compile to nothing —
// measured ~4.5% of steady-state gameplay host time.
// The default headless build keeps them: it is the debugging tool, and CI
// runs it so the checks retain their value.
#ifdef GS_FAST
#define GS_ASSERT(cond)            ((void)0)
#define GS_ASSERTF(cond, fmt, ...) ((void)0)
#else
#define GS_ASSERT(cond) ((cond) ? (void)0 : gs_assert_fail(#cond, __FILE__, __LINE__, __func__))
#define GS_ASSERTF(cond, fmt, ...)                                                                                     \
    ((cond) ? (void)0 : gs_assert_failf(#cond, __FILE__, __LINE__, __func__, (fmt), ##__VA_ARGS__))
#endif

// The guest asked for something the hardware being emulated really does, and
// this emulator has not implemented it.
//
// NOT an assert, and deliberately NOT compiled out by GS_FAST.  An assert says
// "this cannot happen" and earns its removal from the shipping build because a
// correct program never trips one.  This says "this can happen, it is legal,
// and we cannot do it" -- a statement that is just as true in the release
// build, and more useful there, because that is the build a user is running
// when they find the gap.
//
// It is also not a guest-facing error.  Answering the guest -- a SCSI CHECK
// CONDITION, a bus error, a NAK -- claims the request was wrong when it was
// not, and sends whoever is debugging it to look at the driver.  Fault at the
// host, name the missing function, and stop.
//
// Handled rather than fatal: gs_unimplemented_fail prints the banner and the
// same diagnostics an assertion does, then stops the scheduler and returns
// control to the shell.  A dead browser tab tells a user less than a stopped
// machine with a message does.
void gs_unimplemented_fail(const char *file, int line, const char *func, const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 4, 5)))
#endif
    ;

#define GS_UNIMPLEMENTED(fmt, ...) gs_unimplemented_fail(__FILE__, __LINE__, __func__, (fmt), ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif // GS_ASSERT_H
