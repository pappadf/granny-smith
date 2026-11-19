// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// byte_store.c
// The shared byte-store methods -- see byte_store.h.

#include "byte_store.h"

#include <string.h>

// Write n bytes at off: through the store's write hook when it has one (false
// at the first refused byte), else straight into the store.
static bool store_write(const byte_store_ops_t *ops, struct object *self, uint8_t *nv, size_t off, const uint8_t *p,
                        size_t n) {
    if (!ops->write) {
        memcpy(nv + off, p, n); // no policy: a plain copy
        return true;
    }
    for (size_t i = 0; i < n; i++)
        if (!ops->write(self, off + i, p[i]))
            return false; // refused (e.g. write-protected)
    return true;
}

value_t byte_store_peek(const byte_store_ops_t *ops, struct object *self, const value_t *argv) {
    uint8_t *nv = ops->store(self);
    if (!nv)
        return val_err("%s", ops->missing);
    uint64_t addr = argv[0].u;
    if (addr >= ops->size)
        return val_err("%s.peek: offset 0x%llX is outside the %zu-byte store", ops->what, (unsigned long long)addr,
                       ops->size);
    value_t v = val_uint(1, nv[addr]);
    v.flags |= VFLAG_HEX; // a byte of a store reads as hex
    return v;
}

value_t byte_store_poke(const byte_store_ops_t *ops, struct object *self, const value_t *argv) {
    uint8_t *nv = ops->store(self);
    if (!nv)
        return val_err("%s", ops->missing);
    uint64_t addr = argv[0].u;
    const value_t *bytes = &argv[1];
    if (bytes->kind != VK_BYTES || !bytes->bytes.p)
        return val_err("%s.poke: bytes argument must be VK_BYTES (use the :N width suffix, e.g. 0x80:1)", ops->what);
    size_t n = bytes->bytes.n;
    if (n == 0)
        return val_err("%s.poke: bytes argument is empty", ops->what);
    if (addr >= ops->size || addr + n > ops->size)
        return val_err("%s.poke: write of %zu bytes at 0x%llX would overflow the %zu-byte store", ops->what, n,
                       (unsigned long long)addr, ops->size);
    if (!store_write(ops, self, nv, (size_t)addr, bytes->bytes.p, n))
        return val_err("%s.poke: the store is write-protected", ops->what);
    return val_none();
}

value_t byte_store_dump(const byte_store_ops_t *ops, struct object *self, const value_t *argv) {
    uint8_t *nv = ops->store(self);
    if (!nv)
        return val_err("%s", ops->missing);
    uint64_t addr = argv[0].u;
    uint64_t n = argv[1].u;
    if (addr >= ops->size || n == 0 || addr + n > ops->size)
        return val_err("%s.dump: read of %llu bytes at 0x%llX would overflow the %zu-byte store", ops->what,
                       (unsigned long long)n, (unsigned long long)addr, ops->size);
    return val_bytes(nv + addr, (size_t)n);
}

value_t byte_store_snapshot(const byte_store_ops_t *ops, struct object *self) {
    uint8_t *nv = ops->store(self);
    if (!nv)
        return val_err("%s", ops->missing);
    return val_bytes(nv, ops->size);
}

value_t byte_store_restore(const byte_store_ops_t *ops, struct object *self, const value_t *argv) {
    uint8_t *nv = ops->store(self);
    if (!nv)
        return val_err("%s", ops->missing);
    const value_t *bytes = &argv[0];
    if (bytes->kind != VK_BYTES || bytes->bytes.n != ops->size || !bytes->bytes.p)
        return val_err("%s.restore: expected VK_BYTES of length %zu (got len=%zu)", ops->what, ops->size,
                       bytes->kind == VK_BYTES ? bytes->bytes.n : 0);
    if (!store_write(ops, self, nv, 0, bytes->bytes.p, ops->size))
        return val_err("%s.restore: the store is write-protected", ops->what);
    return val_none();
}
