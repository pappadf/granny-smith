// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// dma_mem.h
// One guest-physical memory port for every bus-master engine.
//
// The SONIC, the PSC's seven-channel engine, the AMIC's and Grand Central's
// DBDMA all move bytes the CPU never sees, and each declared its own pair of
// hook typedefs plus its own setter.  Two SHAPES were in use --
// `(phys, value, width)` and `(phys, buf, len)` -- which reads as
// duplication "for no reason".
//
// It is not quite that.  The shapes differ because the engines do: an engine
// that reads a descriptor field wants a width-sized scalar, and one that
// moves kilobytes per command wants a block so the RAM path can be a memcpy
// rather than a per-byte call.  So this file keeps BOTH, in one type, with
// generic fallbacks between them -- an engine implements whichever its
// backing store makes cheap and calls whichever its command makes natural.
//
// What WAS duplication, exactly and literally, is the implementation: the
// eighteen-line pair that forwards to the MMU's physical accessors appeared
// byte-for-byte three times, in q700.c, q900.c and av.c.  That is
// `dma_mem_port_physical` now.
//
// Note what `dma_mem_port_physical` resolves: host memory (RAM/VRAM/ROM)
// only.  mmu_read_physical_* returns 0 for device space rather than
// dispatching, which is why tnt.c's DBDMA port is a DIFFERENT
// implementation -- RAM by memcpy, everything else through
// memory_read_uint8_slow, i.e. the whole bus.  The two are not
// interchangeable and this file does not pretend they are.

#ifndef GS_DMA_MEM_H
#define GS_DMA_MEM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// A port supplies at least one shape of each direction; the accessors below
// bridge to the other.  `ctx` is passed back to every callback.
typedef struct dma_mem_port {
    uint32_t (*read)(void *ctx, uint32_t phys, unsigned width); // width 1/2/4, big-endian
    void (*write)(void *ctx, uint32_t phys, uint32_t value, unsigned width);
    void (*read_block)(void *ctx, uint32_t phys, uint8_t *buf, uint32_t len);
    void (*write_block)(void *ctx, uint32_t phys, const uint8_t *buf, uint32_t len);
    void *ctx;
} dma_mem_port_t;

// Scalar access.  Falls back to the block form a byte at a time, composing
// big-endian, when the port supplies only that.  A port with neither reads
// as zero and drops writes -- the same "nothing is wired here" behaviour an
// engine saw before its hooks were set.
uint32_t dma_mem_read(const dma_mem_port_t *p, uint32_t phys, unsigned width);
void dma_mem_write(const dma_mem_port_t *p, uint32_t phys, uint32_t value, unsigned width);

// Block access.  Falls back to the scalar form a byte at a time.
void dma_mem_read_block(const dma_mem_port_t *p, uint32_t phys, uint8_t *buf, uint32_t len);
void dma_mem_write_block(const dma_mem_port_t *p, uint32_t phys, const uint8_t *buf, uint32_t len);

// True if the port can move anything at all.
bool dma_mem_port_bound(const dma_mem_port_t *p);

// The host-physical port: straight through the MMU's physical accessors,
// which is what q700.c, q900.c and av.c each wrote out by hand.  Host memory
// only -- see the file header.  Stateless, so `ctx` is unused and one
// instance serves every machine.
extern const dma_mem_port_t dma_mem_port_physical;

#ifdef __cplusplus
}
#endif

#endif // GS_DMA_MEM_H
