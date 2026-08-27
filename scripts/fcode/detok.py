#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
#
# detok.py
# An IEEE 1275 FCode detokenizer for PCI expansion ROMs.
#
# Input: a PCI expansion-ROM image (55 AA + PCIR + an Open Firmware FCode
# body) or a raw FCode program.  Output: an annotated listing — token names,
# b(lit) values, string literals, colon-definition boundaries, the FCode
# numbers new-token/named-token allocate, `$call-parent` targets resolved to
# the string that precedes them, and the property-building sequences folded
# back into `name = value`.
#
# Why this exists: the house rule is to disassemble and understand rather
# than boot-and-compare, and a card's own FCode is the most direct statement
# of what its hardware must do.  This tool was written to answer three
# questions about the Apple Accelerated PCI Graphics Card (ATI Mach64 GX,
# "Spinnaker") before a line of device model was written — what properties
# it publishes, which registers its probe touches, and what sequence sits
# behind its "No monitor" bail-out — and it is reusable for every other
# FCode card whose ROM we hold.
#
# Positive control (the detectors-must-fail-loudly rule): the walk must
# consume EXACTLY the byte count the FCode header declares and terminate on
# end0/end1.  Anything else means the operand shapes are wrong somewhere and
# the listing below the divergence is fiction; --strict makes that fatal.
#
# Usage:
#   detok.py ROM                       # annotated listing
#   detok.py ROM --properties          # just the published property list
#   detok.py ROM --registers           # just the register/aperture accesses
#   detok.py ROM --strings             # just the string literals
#   detok.py ROM --extract-fcode OUT   # write the raw FCode program out
#   detok.py ROM --extract-driver OUT  # write the embedded ndrv (PEF) out

import argparse
import struct
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])

from fcode_tokens import DEFINING, FCODE  # noqa: E402
from mach64_regs import io_decode, selftest  # noqa: E402

# ---------------------------------------------------------------------------
# The PCI expansion-ROM container
# ---------------------------------------------------------------------------


class RomError(Exception):
    pass


class PciRom:
    """A PCI expansion ROM: the $55AA signature, the PCI Data Structure and
    the code image the structure describes (PCI 2.x §6.3)."""

    def __init__(self, data):
        self.data = data
        self.is_container = len(data) >= 0x20 and data[0] == 0x55 and data[1] == 0xAA
        self.vendor_id = self.device_id = None
        self.class_code = self.code_type = self.indicator = None
        self.image_size = None
        self.fcode_offset = 0
        if not self.is_container:
            return
        pcir = struct.unpack_from("<H", data, 0x18)[0]
        if pcir + 0x18 > len(data) or data[pcir : pcir + 4] != b"PCIR":
            raise RomError(f"no PCIR signature at ${pcir:04X}")
        self.pcir_offset = pcir
        self.vendor_id, self.device_id = struct.unpack_from("<HH", data, pcir + 4)
        self.class_code = data[pcir + 0x0D] | (data[pcir + 0x0E] << 8) | (data[pcir + 0x0F] << 16)
        self.image_size = struct.unpack_from("<H", data, pcir + 0x10)[0] * 512
        self.code_type = data[pcir + 0x14]
        self.indicator = data[pcir + 0x15]
        # For code type 1 (Open Firmware) the FCode program starts at the
        # offset the image's halfword at $02 carries.
        self.fcode_offset = struct.unpack_from("<H", data, 0x02)[0]

    def fcode_header(self):
        """(format, checksum, length) from the FCode program's own header."""
        off = self.fcode_offset
        d = self.data
        if off + 8 > len(d) or d[off] not in (0xF0, 0xF1, 0xF2, 0xF3):
            raise RomError(f"no start0/1/2/4 token at ${off:04X} (found ${d[off]:02X})")
        fmt = d[off + 1]
        checksum = struct.unpack_from(">H", d, off + 2)[0]
        length = struct.unpack_from(">I", d, off + 4)[0]
        return fmt, checksum, length

    def fcode_body(self):
        """The FCode program, header token included."""
        _, _, length = self.fcode_header()
        off = self.fcode_offset
        return self.data[off : off + length]


# ---------------------------------------------------------------------------
# The detokenizer
# ---------------------------------------------------------------------------


class Op:
    """One decoded token: where it is, what it is, and its operand."""

    __slots__ = ("pc", "code", "name", "operand", "note", "size")

    def __init__(self, pc, code, name, operand=None, note=None, size=1):
        self.pc = pc
        self.code = code
        self.name = name
        self.operand = operand
        self.note = note
        self.size = size


class Detokenizer:
    """Walk an FCode program token by token.

    The one piece of state that changes how the stream is READ is the
    offset16 flag: branch offsets are signed bytes or signed halfwords, and
    an `offset16` token switches an 8-bit program to 16-bit for the rest of
    its length.

    The STARTING width is not something to assume.  It is settled per
    program by the walk verification below — a program walked at the wrong
    width diverges within a few hundred bytes and fails to land on its
    declared end.  `--offset8` / `--offset16` force it; the default tries
    16-bit first (what the tokenizers that produced the Apple/ATI card ROMs
    emit) and falls back to 8-bit, reporting which one verified."""

    def __init__(self, body, base=0, offset16=True):
        self.body = body
        self.base = base  # file offset of body[0], for the listing
        self.offset16 = offset16
        self.ops = []
        self.dictionary = {}  # allocated FCode number -> name (or "")
        self.pending_token = None  # (fcode#, name) awaiting its defining word
        self.terminated = False
        self.consumed = 0

    # --- primitive readers --------------------------------------------------

    def _u8(self, pc):
        return self.body[pc]

    def _fcode_number(self, pc):
        """An FCode number: one byte, or two when the first is $01..$0F."""
        b = self.body[pc]
        if 0x01 <= b <= 0x0F:
            return ((b << 8) | self.body[pc + 1], 2)
        return (b, 1)

    def _string(self, pc):
        n = self.body[pc]
        return (self.body[pc + 1 : pc + 1 + n].decode("latin-1"), 1 + n)

    def _branch(self, pc):
        if self.offset16:
            (v,) = struct.unpack_from(">h", self.body, pc)
            return (v, 2)
        v = self.body[pc]
        return (v - 256 if v >= 0x80 else v, 1)

    # --- the walk -----------------------------------------------------------

    def run(self):
        pc = 0
        n = len(self.body)
        # The header token plus its 7 header bytes.
        if n >= 8 and self.body[0] in (0xF0, 0xF1, 0xF2, 0xF3):
            name = FCODE[self.body[0]][0]
            self.ops.append(Op(0, self.body[0], name, note="FCode program header", size=8))
            pc = 8
        while pc < n:
            start = pc
            code, adv = self._fcode_number(pc)
            pc += adv
            entry = FCODE.get(code)
            if entry is None:
                # Not a standard assignment.  It is either a word this
                # program defined (allocated by new-token/named-token) or
                # genuinely unknown — say which, never guess.
                if code in self.dictionary:
                    label = self.dictionary[code] or f"fn_${code:03X}"
                    self.ops.append(Op(start, code, label, note="local", size=pc - start))
                else:
                    self.ops.append(Op(start, code, f"[fcode-${code:03X}]", note="UNASSIGNED", size=pc - start))
                continue
            name, shape = entry
            operand = None
            if shape == "lit":
                (operand,) = struct.unpack_from(">I", self.body, pc)
                pc += 4
            elif shape == "byte":
                operand = self.body[pc]
                pc += 1
            elif shape == "fcode":
                operand, adv = self._fcode_number(pc)
                pc += adv
            elif shape == "str":
                operand, adv = self._string(pc)
                pc += adv
            elif shape == "branch":
                operand, adv = self._branch(pc)
                pc += adv
            elif shape == "strfc":
                text, adv = self._string(pc)
                pc += adv
                num, adv = self._fcode_number(pc)
                pc += adv
                operand = (text, num)
            op = Op(start, code, name, operand, size=pc - start)
            self.ops.append(op)

            # Dictionary bookkeeping: new-token / named-token / external-token
            # allocate a number that the NEXT defining token consumes.
            if name == "new-token":
                self.pending_token = (operand, "")
            elif name in ("named-token", "external-token"):
                self.pending_token = (operand[1], operand[0])
            elif name in DEFINING and self.pending_token is not None:
                num, label = self.pending_token
                self.dictionary[num] = label
                op.note = f"defines {label or ''}${num:03X}".strip()
                self.pending_token = None
            elif name == "offset16":
                self.offset16 = True
            elif name in ("end0", "end1"):
                self.terminated = True
                self.consumed = pc
                break
        if not self.terminated:
            self.consumed = pc
        return self.ops

    # --- the positive control ----------------------------------------------

    def verify(self, declared_length):
        """Did the walk land exactly where the FCode header says it should?

        Returns (problems, warnings).

        A *problem* means the walk itself is wrong: a correct walk consumes
        every declared byte and stops on end0/end1, so a short or long walk
        proves an operand shape is misread upstream and every line after
        the divergence is fiction.  That is the hard gate.

        A *warning* means the walk is sound but a token has no name in the
        table — the listing is still byte-accurate, one line just reads
        `[fcode-$NNN]`.  Worth saying, not worth failing."""
        problems, warnings = [], []
        if not self.terminated:
            problems.append("stream ran off the end without an end0/end1 token")
        if declared_length and self.consumed != declared_length:
            problems.append(
                f"consumed {self.consumed} bytes, header declares {declared_length}"
                f" (delta {self.consumed - declared_length:+d})"
            )
        unassigned = sorted({o.code for o in self.ops if o.note == "UNASSIGNED"})
        if unassigned:
            names = ", ".join(f"${c:03X}" for c in unassigned)
            warnings.append(f"{len(unassigned)} token number(s) have no table entry: {names}")
        return problems, warnings


# ---------------------------------------------------------------------------
# Semantic passes over the decoded stream
# ---------------------------------------------------------------------------


def literal_stack(ops, i, depth):
    """The last `depth` pushed constants before op i, newest last, or None.

    Deliberately naive: it walks backwards over b(lit) and the small
    constant tokens only, and gives up the moment anything else appears.
    That is exactly the shape the 1275 property builders and the parent
    calls have, and refusing to guess past it is the point."""
    out = []
    j = i - 1
    consts = {"0": 0, "1": 1, "2": 2, "3": 3, "-1": 0xFFFFFFFF}
    while j >= 0 and len(out) < depth:
        op = ops[j]
        if op.name == "b(lit)":
            out.append(op.operand)
        elif op.name in consts:
            out.append(consts[op.name])
        else:
            return None
        j -= 1
    if len(out) < depth:
        return None
    return list(reversed(out))


def collect_strings(ops):
    """Every b(") literal with the offset it sits at."""
    return [(o.pc, o.operand) for o in ops if o.name == 'b(")']


def collect_properties(ops):
    """Fold `... " name" property` sequences into (offset, name, summary).

    Also recognises the three shorthand property words — device-name, model
    and device-type — which take the string alone."""
    out = []
    for i, op in enumerate(ops):
        if op.name in ("device-name", "model", "device-type"):
            if i and ops[i - 1].name == 'b(")':
                key = {"device-name": "name", "model": "model", "device-type": "device_type"}[op.name]
                out.append((op.pc, key, f'"{ops[i - 1].operand}"'))
            continue
        if op.name != "property":
            continue
        if not (i and ops[i - 1].name == 'b(")'):
            out.append((op.pc, "?", "(property name not a literal)"))
            continue
        name = ops[i - 1].operand
        # The value is whatever the encode-* chain before the name built.
        # Summarise the shape rather than pretending to evaluate it.
        out.append((op.pc, name, describe_property_value(ops, i - 1)))
    return out


def describe_property_value(ops, name_idx):
    """Summarise the encode-* chain that built the value for a property.

    Walks back from the property-name literal to the start of the encoding
    sequence and reports the encode words used plus the literals they
    consumed — enough to read `reg` off the listing, without inventing an
    evaluator."""
    j = name_idx - 1
    encoders = {"encode-int", "encode+", "encode-phys", "encode-string", "encode-bytes"}
    parts = []
    lits = []
    while j >= 0:
        op = ops[j]
        if op.name in encoders:
            parts.append(op.name)
        elif op.name == "b(lit)":
            lits.append(f"${op.operand:08X}")
        elif op.name == 'b(")':
            lits.append(f'"{op.operand}"')
        elif op.name in ("0", "1", "2", "3", "-1", "my-address", "my-space", "or", "swap", "dup", "drop"):
            pass
        else:
            break
        j -= 1
    if not parts:
        return "(built at run time)"
    lits.reverse()
    return f"{'+'.join(reversed(parts))} over [{', '.join(lits) if lits else 'run-time values'}]"


def collect_parent_calls(ops):
    """Every `" method" $call-parent` site, with the literals that precede it."""
    out = []
    for i, op in enumerate(ops):
        if op.name != "$call-parent":
            continue
        method = ops[i - 1].operand if i and ops[i - 1].name == 'b(")' else "?"
        args = literal_stack(ops, i - 1, 4)
        out.append((op.pc, method, args))
    return out


def collect_register_refs(ops):
    """Every b(lit) that decodes as a mach64 sparse-I/O register address,
    plus the aperture-relative register-block bases."""
    out = []
    for op in ops:
        if op.name != "b(lit)":
            continue
        v = op.operand
        hit = io_decode(v)
        if hit:
            name, sel, base, byte = hit
            suffix = "" if byte == 0 else f" +{byte}"
            out.append((op.pc, v, f"{name} (I/O sel ${sel:02X}, base ${base:03X}){suffix}"))
        elif v in (0x7FFC00, 0x3FFC00):
            size = "8 MB" if v == 0x7FFC00 else "4 MB"
            out.append((op.pc, v, f"memory-mapped register block ({size} aperture)"))
    return out


# ---------------------------------------------------------------------------
# Listing
# ---------------------------------------------------------------------------


def format_op(op, base, dictionary):
    addr = base + op.pc
    raw = f"{addr:06X}"
    if op.name == "b(lit)":
        hit = io_decode(op.operand)
        ann = ""
        if hit:
            ann = f"   \\ {hit[0]}" + (f" +{hit[3]}" if hit[3] else "")
        elif op.operand in (0x7FFC00, 0x3FFC00):
            ann = "   \\ memory-mapped register block"
        return f"{raw}  b(lit) ${op.operand:08X}{ann}"
    if op.name == 'b(")':
        return f'{raw}  b(") "{op.operand}"'
    if op.name in ("named-token", "external-token"):
        return f'{raw}  {op.name} "{op.operand[0]}" ${op.operand[1]:03X}'
    if op.name == "new-token":
        return f"{raw}  new-token ${op.operand:03X}"
    if op.name == "b(to)":
        target = dictionary.get(op.operand, "")
        return f"{raw}  b(to) {target or ''}${op.operand:03X}".rstrip()
    if op.name == "b(')":
        target = dictionary.get(op.operand, "")
        return f"{raw}  b(') {target or ''}${op.operand:03X}".rstrip()
    if op.operand is not None and isinstance(op.operand, int):
        # A branch offset: print the destination too, it is what a reader wants.
        dest = base + op.pc + op.size + op.operand if op.name.startswith("b") else None
        if dest is not None:
            return f"{raw}  {op.name} {op.operand:+d}   \\ -> {dest:06X}"
        return f"{raw}  {op.name} {op.operand}"
    note = f"   \\ {op.note}" if op.note else ""
    return f"{raw}  {op.name}{note}"


def extract_driver(ops):
    """Reassemble the embedded Mac OS ndrv from the FCode token stream.

    The PEF is NOT contiguous in the ROM.  It is published as the
    `driver,AAPL,MacOS,PowerPC` property, built by a long chain of
    encode-bytes over ordinary b(") string literals of at most 255 bytes
    each — which is why the token walk passes straight through it instead
    of needing to skip a data blob, and why `strings` on the ROM finds the
    PEF header but not the driver.

    Collect every literal from the one carrying the `Joy!peff` container
    magic up to (not including) the property-name literal.  Returns
    (bytes, literal_count) or (None, 0)."""
    lits = [(i, o) for i, o in enumerate(ops) if o.name == 'b(")']
    start = next((i for i, o in lits if o.operand.startswith("Joy!peff")), None)
    end = next((i for i, o in lits if o.operand == "driver,AAPL,MacOS,PowerPC"), None)
    if start is None:
        return (None, 0)
    blob = bytearray()
    n = 0
    for i, o in lits:
        if i >= start and (end is None or i < end):
            blob += o.operand.encode("latin-1")
            n += 1
    return (bytes(blob), n)


def describe_pef(blob):
    """Summarise a PEF container, and CHECK it — the section table has to
    tile the file exactly, which is what proves the reassembly above put the
    pieces back in the right order with nothing lost or duplicated."""
    out = []
    if len(blob) < 40 or blob[:8] != b"Joy!peff":
        return ["not a PEF container"]
    arch = blob[8:12].decode("latin-1")
    nsec = struct.unpack_from(">H", blob, 32)[0]
    out.append(f"PEF arch={arch} sections={nsec} size={len(blob)}")
    kinds = {0: "code", 1: "unpackedData", 2: "patternData", 3: "constant", 4: "loader"}
    end_of_last = 0
    for i in range(nsec):
        o = 40 + i * 28
        _, _, total, _, clen, coff, kind = struct.unpack_from(">iIIIIIB", blob, o)
        out.append(f"[{i}] {kinds.get(kind, kind):12s} containerOff=${coff:06X} len={clen} total={total}")
        end_of_last = max(end_of_last, coff + clen)
    out.append(f"section table tiles to {end_of_last} of {len(blob)} bytes"
               + ("  OK" if end_of_last == len(blob) else "  MISMATCH — reassembly is wrong"))
    return out


def main():
    ap = argparse.ArgumentParser(description="IEEE 1275 FCode detokenizer for PCI expansion ROMs")
    ap.add_argument("rom", help="PCI expansion-ROM image, or a raw FCode program")
    ap.add_argument("--raw", action="store_true", help="input is a raw FCode program, not a PCI ROM")
    ap.add_argument("--properties", action="store_true", help="list the published properties only")
    ap.add_argument("--registers", action="store_true", help="list mach64 register references only")
    ap.add_argument("--strings", action="store_true", help="list string literals only")
    ap.add_argument("--calls", action="store_true", help="list $call-parent sites only")
    ap.add_argument("--extract-fcode", metavar="OUT", help="write the raw FCode program to OUT")
    ap.add_argument("--extract-driver", metavar="OUT",
                    help="write the embedded Mac OS ndrv (a PEF container) to OUT")
    ap.add_argument("--offset8", action="store_true", help="force 8-bit branch offsets")
    ap.add_argument("--offset16", action="store_true", help="force 16-bit branch offsets")
    ap.add_argument("--strict", action="store_true", help="exit non-zero if the walk does not verify")
    args = ap.parse_args()

    selftest()  # the register table's own positive control

    with open(args.rom, "rb") as f:
        data = f.read()

    if args.raw:
        body, base, declared = data, 0, 0
        header = None
    else:
        rom = PciRom(data)
        if not rom.is_container:
            print(f"{args.rom}: no $55AA signature — pass --raw for a bare FCode program", file=sys.stderr)
            return 2
        header = rom
        body = rom.fcode_body()
        base = rom.fcode_offset
        _, _, declared = rom.fcode_header()

    if args.extract_fcode:
        with open(args.extract_fcode, "wb") as f:
            f.write(body)
        print(f"wrote {len(body)} bytes of FCode to {args.extract_fcode}")
        return 0

    if args.extract_driver:
        dt = Detokenizer(body, base, offset16=not args.offset8)
        ops = dt.run()
        blob, n = extract_driver(ops)
        if blob is None:
            print(f"{args.rom}: no embedded ndrv found", file=sys.stderr)
            return 2
        with open(args.extract_driver, "wb") as f:
            f.write(blob)
        print(f"wrote {len(blob)} bytes of ndrv (from {n} string literals) to {args.extract_driver}")
        for line in describe_pef(blob):
            print(f"  {line}")
        return 0

    # Settle the branch-offset width by walking, not by assuming: the wrong
    # width diverges and fails to land on the program's declared end.
    dt = Detokenizer(body, base, offset16=not args.offset8)
    ops = dt.run()
    problems, warnings = dt.verify(declared)
    if problems and not args.offset8 and not args.offset16:
        alt = Detokenizer(body, base, offset16=False)
        alt_ops = alt.run()
        alt_problems, alt_warnings = alt.verify(declared)
        if not alt_problems:
            dt, ops, problems, warnings = alt, alt_ops, alt_problems, alt_warnings

    selective = args.properties or args.registers or args.strings or args.calls
    if not selective:
        if header:
            fmt, cksum, length = header.fcode_header()
            print(f"# {args.rom}")
            print(f"# PCI expansion ROM: vendor ${header.vendor_id:04X} device ${header.device_id:04X} "
                  f"class ${header.class_code:06X}")
            print(f"# image {header.image_size} bytes, code type ${header.code_type:02X} "
                  f"({'Open Firmware' if header.code_type == 1 else 'NOT Open Firmware'}), "
                  f"indicator ${header.indicator:02X}")
            print(f"# FCode at ${base:04X}: format ${fmt:02X} checksum ${cksum:04X} length {length} bytes")
            print()
        for op in ops:
            print(format_op(op, base, dt.dictionary))
        print()

    if args.properties or not selective:
        print("# Published properties")
        for off, name, value in collect_properties(ops):
            print(f"  ${base + off:06X}  {name:<28} {value}")
        print()

    if args.calls or not selective:
        print("# Parent-package calls")
        for off, method, argv in collect_parent_calls(ops):
            shown = ", ".join(f"${a:08X}" for a in argv) if argv else "(non-literal arguments)"
            print(f"  ${base + off:06X}  {method:<14} [{shown}]")
        print()

    if args.registers or not selective:
        print("# mach64 register references (b(lit) values that decode as addresses)")
        for off, value, what in collect_register_refs(ops):
            print(f"  ${base + off:06X}  ${value:08X}  {what}")
        print()

    if args.strings or not selective:
        print("# String literals")
        for off, text in collect_strings(ops):
            print(f"  ${base + off:06X}  \"{text}\"")
        print()

    print("# Walk verification (the positive control)")
    print(f"  tokens decoded : {len(ops)}")
    print(f"  bytes consumed : {dt.consumed} of {declared} declared")
    print(f"  terminated     : {'yes' if dt.terminated else 'NO'}")
    print(f"  branch offsets : {'16-bit' if dt.offset16 else '8-bit'}")
    print(f"  words defined  : {len(dt.dictionary)}")
    for w in warnings:
        print(f"  note: {w}")
    if problems:
        for msg in problems:
            print(f"  PROBLEM: {msg}")
        print("  The listing above is NOT trustworthy past the first divergence.")
        if args.strict:
            return 1
    else:
        print("  OK — the walk consumed exactly the declared program and ended on a terminator.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
