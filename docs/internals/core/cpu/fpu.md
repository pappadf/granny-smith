# Motorola 68882 FPU Emulation

## Overview

The emulator includes a software model of the **Motorola 68882 Floating-Point Coprocessor** used in the Macintosh SE/30 (68030 CPU). The 68882 is accessed through the 68020/68030 coprocessor interface: F-line instructions (opcode bits 15:12 = `1111`) with CpID=1 (bits 11:9 = `001`) are decoded by the CPU, which then dispatches to the FPU module. The emulator bypasses the hardware coprocessor bus protocol and executes FPU operations inline, the same approach used for the MMU (CpID=0).

System 7 probes for the FPU at boot via `FBcc`/`FSAVE` instructions and crashes if the coprocessor interface does not respond correctly, making a functional 68882 model essential for SE/30 emulation.

**Key design goals:**

- Bit-exact compatibility with the 68882 hardware
- Platform-independent soft-float arithmetic (no reliance on host `long double`)
- Full instruction coverage: all arithmetic, transcendental, data movement, and control flow operations
- Correct exception model with pre- and post-instruction exception checks

## File Layout

| File | Responsibility |
|------|----------------|
| `src/core/cpu/fpu.h` | `float80_reg_t`, `fpu_unpacked_t`, `fpu_state_t`, public API, predicate helpers |
| `src/core/cpu/fpu.c` | Soft-float core: 128-bit primitives, pack/unpack, arithmetic, format conversions, FMOVE, FMOVEM, FMOVECR, FSAVE/FRESTORE, exception logic, operation dispatch |
| `src/core/cpu/fpu_transc.c` | Transcendental functions: trig, exp/log, hyperbolic, inverse trig — FPSP-derived polynomial approximations |
| `src/core/cpu/cpu_ops.h` | FPU instruction dispatch macros (`OP_FPU_GENERAL`, `OP_FPU_SCCDBCC`, `OP_FSAVE_EA`, `OP_FRESTORE_EA`) |
| `src/core/cpu/cpu_decode.h` | F-line decode (cases `0x08`, `0x09` for CpID=1 type 0 and type 1) |

### Boundary: cpu_ops.h vs fpu.c

**In `cpu_ops.h`** (control flow, supervisor-level):
- `FSAVE` / `FRESTORE` (state frame save/restore, supervisor-only)
- `FBcc`, `FScc`, `FDBcc`, `FTRAPcc` (condition evaluation + branch/set/trap)
- Post-instruction FPU exception vectoring

**In `fpu.c`** (arithmetic, data movement):
- All arithmetic operations (`FADD` through `FSINCOS`)
- Data movement: `FMOVE` (all directions), `FMOVEM` (data + control), `FMOVECR`
- Format conversions: integer, single, double, extended, packed decimal
- Condition code computation from `float80_reg_t`

## Register Storage: `float80_reg_t`

The eight FP data registers (FP0–FP7) use a storage format that maps directly to the 68882's 80-bit extended precision layout:

```c
typedef struct {
    uint16_t exponent; // bit 15 = sign, bits 14:0 = biased exponent (bias=16383)
    uint64_t mantissa; // 64-bit mantissa (bit 63 = explicit integer/J bit)
} float80_reg_t;
```

The 68882 extended format in memory is 96 bits (12 bytes): 16-bit sign+exponent, 16-bit zero padding, 64-bit mantissa. The `float80_reg_t` stores the 80 significant bits without the padding.

An earlier proposal used `double` as the internal register type. This was rejected because the host `double` has a 53-bit mantissa (vs. the 68882's 64-bit mantissa), causing precision loss on every register load/store cycle, incorrect rounding, and misbehavior with denormals, NaN payloads, and signed zeros.

### Special Encodings

| Exponent (15-bit) | Mantissa | Meaning |
|--------------------|----------|---------|
| `0x0000` | `0` | Positive/negative zero (sign determines) |
| `0x0000` | `!= 0` | Denormalized number (exponent = −16382) |
| `0x7FFF` | `0` | Positive/negative infinity |
| `0x7FFF` | bit 63=1, bit 62=1 | Quiet NaN |
| `0x7FFF` | bit 63=1, bit 62=0 | Signaling NaN |
| `0x0001`–`0x7FFE` | bit 63=1 | Normal number |
| `0x0001`–`0x7FFE` | bit 63=0 | Unnormalized number (68882 accepts these) |

Predicate helpers (`fp80_is_zero`, `fp80_is_inf`, `fp80_is_nan`, `fp80_is_snan`, `fp80_is_denormal`, `fp80_is_negative`) classify values by inspecting the exponent and mantissa fields.

## Internal Calculation Format: `fpu_unpacked_t`

During arithmetic, operands are "unpacked" into a wider format with extra bits for intermediate precision:

```c
typedef struct {
    bool     sign;         // sign bit
    int32_t  exponent;     // signed, unbiased exponent
    uint64_t mantissa_hi;  // upper 64 bits (bit 63 = integer/J bit)
    uint64_t mantissa_lo;  // lower 64 bits (guard + round + sticky)
} fpu_unpacked_t;
```

### Why 128-bit Mantissa

- **Multiplication** of two 64-bit mantissas produces a 128-bit result. Without the low 64 bits, guard/round/sticky information is lost and rounding cannot be exact.
- **Addition/subtraction** with large exponent differences shifts one operand right; the shifted-out bits accumulate in `mantissa_lo` for correct rounding.
- **Division** iterative algorithms need extra working bits to converge to 64-bit precision.

### Why Wider Exponent

The 15-bit biased exponent (range 0–32767, unbiased −16383 to +16384) is widened to `int32_t` so intermediate results can temporarily exceed the representable range. Overflow/underflow detection happens at the final rounding (pack) stage, not during intermediate computation.

### Sentinel Values

| Constant | Value | Meaning |
|----------|-------|---------|
| `FPU_EXP_ZERO` | `−32768` | Zero (mantissa=0) or denormal (mantissa≠0) |
| `FPU_EXP_INF` | `32767` | Infinity (mantissa=0) or NaN (mantissa≠0) |
| `FPU_EXP_BIAS` | `16383` | 68882 extended-precision exponent bias |

### Pack and Unpack

```c
fpu_unpacked_t fpu_unpack(float80_reg_t reg);   // lossless: float80 → unpacked
float80_reg_t  fpu_pack(fpu_state_t *fpu, fpu_unpacked_t val); // with rounding
```

`fpu_unpack()` widens a register value to the unpacked format without precision loss. `fpu_pack()` is where rounding mode (FPCR bits 5:4) and precision control (FPCR bits 7:6) are applied. It also detects overflow, underflow, and inexact results, setting the appropriate FPSR exception bits.

## FPU State Structure

```c
typedef struct fpu_state {
    float80_reg_t fp[8];              // FP0–FP7 data registers
    uint32_t fpcr;                    // FPU control register
    uint32_t fpsr;                    // FPU status register
    uint32_t fpiar;                   // FPU instruction address register
    uint32_t pre_exc_mask;            // exceptions already fired as pre-instruction
    bool initialized;                 // true once any FPU op has executed (for FSAVE)
    float80_reg_t exceptional_operand; // last operand that caused an exception
} fpu_state_t;
```

On hardware reset, data registers are initialized to non-signaling (quiet) NaNs (`0x7FFF:0xFFFFFFFFFFFFFFFF`) per MC68882UM §2.2. Control registers are zeroed.

### FPCR Layout

| Bits | Field | Description |
|------|-------|-------------|
| 15:8 | Exception enables | BSUN, SNAN, OPERR, OVFL, UNFL, DZ, INEX2, INEX1 |
| 7:6 | Rounding precision | 00=extended, 01=single, 10=double |
| 5:4 | Rounding mode | 00=nearest, 01=zero, 10=−∞, 11=+∞ |

### FPSR Layout

| Bits | Field | Description |
|------|-------|-------------|
| 27:24 | Condition code | N, Z, I, NaN |
| 23:16 | Quotient | Sign + 7-bit quotient (from FMOD/FREM) |
| 15:8 | Exception status | Per-operation exceptions (cleared by next op) |
| 7:3 | Accrued exceptions | Sticky exception bits |

## 128-bit Integer Primitives

The soft-float core is built on portable 128-bit integer helpers:

- `uint128_add()` / `uint128_sub()` — 128-bit unsigned add/subtract
- `uint64_mul128()` — 64×64→128 unsigned multiply. Uses `__uint128_t` when available (GCC/Clang on 64-bit hosts), with a portable four-32×32 fallback for Emscripten/WASM.
- `uint128_shr()` / `uint128_shl()` — 128-bit shift right/left
- `uint128_shr_sticky()` — right shift with sticky bit (shifted-out nonzero bits OR into LSB)

These primitives ensure identical behavior across x86-64, aarch64, and WebAssembly targets.

## Core Arithmetic

All arithmetic operates on `fpu_unpacked_t` values using pure integer algorithms:

| Function | Description |
|----------|-------------|
| `fpu_op_add()` | Addition with sign/special-case handling (NaN propagation, ∞±∞) |
| `fpu_op_sub()` | Subtraction (negate + add) |
| `fpu_op_mul()` | Multiply via 64×64→128, normalize, round |
| `fpu_op_div()` | Division via iterative shift-and-subtract |
| `fpu_op_sqrt()` | Square root via bit-by-bit algorithm |
| `fpu_op_rem()` / `fpu_op_mod()` | IEEE remainder (FREM) / modulo (FMOD) with quotient in FPSR |
| `fpu_normalize()` | Shift mantissa left until J-bit set, decrementing exponent |

Each function handles special cases (NaN propagation, infinity arithmetic, zero rules) before performing the mantissa computation.

**FMOD vs FREM:** Both use iterative shift-and-subtract on 128-bit mantissas. FMOD truncates the quotient toward zero; FREM rounds the quotient to nearest. Both store the 7-bit quotient + sign in FPSR bits 23:16.

## Instruction Dispatch

All FPU instructions are F-line (bits 15:12 = `1111`) with CpID=1 (bits 11:9 = `001`). Bits 8:6 (type field) determine the instruction class:

| Type | Bits 8:6 | Instructions |
|------|----------|--------------|
| 0 | `000` | General: FMOVE, arithmetic, FMOVEM, FMOVECR |
| 1 | `001` | FScc, FDBcc, FTRAPcc |
| 2 | `010` | FBcc.W |
| 3 | `011` | FBcc.L |
| 4 | `100` | FSAVE |
| 5 | `101` | FRESTORE |

### Type 0 Extension Word

For type 0, the extension word (second instruction word) determines the operation. Bits 15:13 (`top3`) are the primary dispatch selector:

| top3 | Meaning |
|------|---------|
| `0b000` | FPn→FPn: `FOP.X FPs,FPd` |
| `0b010` | EA→FPn: `FOP.fmt <ea>,FPd` |
| `0b011` | FPn→EA: `FMOVE.fmt FPn,<ea>` |
| `0b100` / `0b101` | FMOVEM control registers |
| `0b110` / `0b111` | FMOVEM data registers (static/dynamic list) |

For register-to-register with source specifier `10111` (src_spec=7, dir=1), the instruction is `FMOVECR` (load ROM constant).

### Operation Codes (bits 6:0)

The 7-bit opcode in the extension word selects from ~40 operations including:

- **Data movement:** FMOVE (0x00)
- **Integer rounding:** FINT (0x01), FINTRZ (0x03)
- **Basic arithmetic:** FADD (0x22), FSUB (0x28), FMUL (0x23), FDIV (0x20), FSQRT (0x04)
- **Single-precision variants:** FSGLDIV (0x24), FSGLMUL (0x27)
- **Comparison:** FCMP (0x38), FTST (0x3A)
- **Extraction:** FGETEXP (0x1E), FGETMAN (0x1F), FSCALE (0x26)
- **Remainder:** FMOD (0x21), FREM (0x25)
- **Sign:** FABS (0x18), FNEG (0x1A)
- **Transcendentals:** FSIN (0x0E), FCOS (0x1D), FSINCOS (0x30–0x37), FTAN (0x0F), FATAN (0x0A), FASIN (0x0C), FACOS (0x1C), FSINH (0x02), FCOSH (0x19), FTANH (0x09), FATANH (0x0D), FETOX (0x10), FETOXM1 (0x08), FTWOTOX (0x11), FTENTOX (0x12), FLOGN (0x14), FLOGNP1 (0x06), FLOG10 (0x15), FLOG2 (0x16)

### General Op Dispatcher (`fpu_general_op`)

The top-level entry point for type 0 instructions:

1. Pre-instruction exception check (MC68882UM §6.1.4)
2. Clear per-operation exception status bits in FPSR
3. Dispatch on `top3`:
   - **0/2**: Read source operand (from FPn or EA), call `fpu_execute_op()` with 7-bit opcode
   - **1**: FMOVECR — load ROM constant to FPd
   - **3**: FMOVE FPn→memory — convert and store to EA
   - **4/5**: FMOVEM control registers
   - **6/7**: FMOVEM data registers
4. Post-instruction exception check (for FMOVE to memory only)

FPIAR is updated only for instructions that can generate FPU exceptions (arithmetic, FMOVE, FCMP, FTST, FMOVECR). FMOVEM, FSAVE, and FRESTORE do not update FPIAR.

## Data Format Conversions

The FPU supports these source/destination formats, encoded in bits 12:10 of the extension word:

| Code | Format | Size | Direction |
|------|--------|------|-----------|
| 000 | Long Integer (L) | 4 bytes | Load/Store |
| 001 | Single (S) | 4 bytes | Load/Store |
| 010 | Extended (X) | 12 bytes | Load/Store |
| 011 | Packed Decimal (P) | 12 bytes | Load/Store |
| 100 | Word Integer (W) | 2 bytes | Load/Store |
| 101 | Double (D) | 8 bytes | Load/Store |
| 110 | Byte Integer (B) | 1 byte | Load/Store |

### Packed Decimal (BCD)

Packed decimal is a 96-bit (12-byte) format used by `FMOVE.P`:

- Byte 0: sign of number (bit 7), sign of exponent (bit 6)
- Bytes 0–1: 12-bit BCD exponent (3 digits)
- Bytes 2–11: 17-digit BCD mantissa (most significant digit in byte 2, two digits per byte thereafter)

Conversion uses the FMOVECR power-of-10 table (10^1 through 10^4096) for scaling. Static or dynamic k-factor (from extension word or data register) controls the number of significant output digits.

## FMOVEM

FMOVEM handles bulk save/restore of FP data and control registers.

**Data registers:** Save/restore FP register lists to/from memory in extended format (12 bytes per register). Supports:
- Static register list (from extension word bits 7:0)
- Dynamic register list (from Dn)
- Predecrement mode `-(An)` (reversed bit order, saves high-numbered registers first)
- Postincrement mode `(An)+` (forward order, restores low-numbered registers first)

**Control registers:** Save/restore FPCR, FPSR, FPIAR as 32-bit values, selected by a 3-bit mask in the extension word.

FMOVEM does not update FPIAR or accrued exception bits.

## FMOVECR ROM Constants

`FMOVECR` loads one of 22 built-in constants into an FP register. All constants are stored with full 67-bit precision as `fpu_unpacked_t` values:

| Offset | Constant |
|--------|----------|
| 0x00 | π |
| 0x0B | log₁₀(2) |
| 0x0C | e |
| 0x0D | log₂(e) |
| 0x0E | log₁₀(e) |
| 0x0F | 0.0 |
| 0x30 | ln(2) |
| 0x31 | ln(10) |
| 0x32–0x3F | Powers of 10: 10⁰, 10¹, 10², 10⁴, 10⁸, … 10⁴⁰⁹⁶ |

## FSAVE / FRESTORE

### FSAVE

Writes a coprocessor state frame to memory. Since the emulator executes all FPU operations atomically (no pipelining), the FPU is always "idle" after an operation. Two frame types are emitted:

- **Null frame** (4 bytes: `$00000000`): FPU has been reset and no operation has executed since
- **Idle frame** (version `$1F`, size `$38` = 56 bytes payload): FPU has been used. The payload encodes enough state to reconstruct the FPU's context, including the `pre_exc_mask` and exceptional operand.

### FRESTORE

- **Null frame** (size=0): Reset FPU — clear all registers, FPCR, FPSR to zero; re-initialize data registers to quiet NaNs
- **Idle frame** (size=`$38`): Restore internal FPU state from payload
- **Busy frame** (size=`$B4`): Accepted and treated as idle (pipeline state not modeled)
- **Invalid version/size**: Format Error exception

## Exception Model

### Per-Operation Exceptions

After each FPU arithmetic operation, the operation sets exception status bits in FPSR (bits 15:8). The corresponding accrued exception bits (FPSR bits 7:3) are OR'd in.

| Exception | FPSR bit | Accrued bit | Vector offset |
|-----------|----------|-------------|---------------|
| BSUN | 15 | IOP (7) | `$C0` (vector 48) |
| SNAN | 14 | IOP (7) | `$D8` (vector 54) |
| OPERR | 13 | IOP (7) | `$D0` (vector 52) |
| OVFL | 12 | OVFL (6) | `$D4` (vector 53) |
| UNFL | 11 | UNFL (5) | `$CC` (vector 51) |
| DZ | 10 | DZ (4) | `$C8` (vector 50) |
| INEX2 | 9 | INEX (3) | `$C4` (vector 49) |
| INEX1 | 8 | INEX (3) | `$C4` (vector 49) |

### Pre-Instruction Exception Check

Per MC68882UM §6.1.4, before each FPU instruction (except FSAVE/FRESTORE), enabled exceptions pending from a previous operation are checked. The `pre_exc_mask` field in `fpu_state_t` tracks which exceptions have already been taken as pre-instruction exceptions, preventing re-firing after a handler acknowledges them.

For conditional instructions (FBcc, FScc, FDBcc, FTRAPcc), the check always fires even for UNFL without INEX2.

### Post-Instruction Exception Check

After the operation completes, FPSR status bits are checked against FPCR enable bits. If any enabled exception occurred, the appropriate exception vector is taken.

### NaN Propagation

- Signaling NaN: set SNAN exception, convert to quiet NaN, propagate
- Two NaN operands: propagate per 68882 convention
- If SNAN enable is set in FPCR, exception fires before conversion

## Transcendental Functions

Transcendental functions are implemented in `fpu_transc.c` (~3400 lines) using algorithms derived from the **Motorola FPSP** (Floating Point Software Package).

### General Approach

1. **Argument reduction** to a small primary range using Cody-Waite or table-driven techniques
2. **Polynomial/rational approximation** using minimax coefficients from the FPSP
3. **Reconstruction** from the reduced result
4. **Precision maintenance** via `fp_rnd64()`, which rounds 128-bit mantissa to 64 bits after each intermediate step, simulating the 68882's register precision at each pipeline stage

### Coefficient Tables

FPSP coefficients are embedded as static constant arrays, constructed from the original FPSP source using helper functions:
- `fpsp_ext(w0, w1, w2)` — build `fpu_unpacked_t` from FPSP extended-precision triple
- `fpsp_dbl(hi, lo)` — build from IEEE 754 double (two big-endian uint32 halves)
- `fpsp_sgl(w)` — build from IEEE 754 single

### Key Lookup Tables

| Table | Entries | Used by |
|-------|---------|---------|
| `LOGTBL` | 64 × 3 | (1/F, log(F)) pairs for FLOGN, FLOGNP1 |
| `EXPTBL` | 64 × 4 | 2^(J/64) + correction tail for FETOX, FETOXM1 |
| `stwotox_tbl` | 64 × 4 | 2^(J/64) + correction for FTWOTOX, FTENTOX |
| `PITBL` | 65 × 4 | N×π/2 lead+trail for FSIN, FCOS, FSINCOS, FTAN |
| `atantbl_data` | 128 × 3 | arctan(F) values for FATAN |

### Implemented Functions

| Function | Opcode | Algorithm |
|----------|--------|-----------|
| FLOGN | 0x14 | Table-driven: K×ln(2) + log(F) + log(1+U) via degree-6 poly; near-1 path uses U=2(X−1)/(X+1) Horner poly |
| FLOG10 | 0x15 | ln(X) × (1/ln(10)) |
| FLOG2 | 0x16 | Exact power-of-2 shortcut; otherwise ln(X) × (1/ln(2)) |
| FLOGNP1 | 0x06 | Three paths: tiny (→X), near-1 (odd poly in U=2X/(1+X)), general (LOGMAIN) |
| FETOX | 0x10 | Cody-Waite reduction: N=round(64X), R=X−N×L1−N×L2, degree-5 poly, EXPTBL reconstruction, scale by 2^M |
| FETOXM1 | 0x08 | Paths: tiny (→X), small (degree-12 Horner), large (delegate to FETOX or →−1), standard (reduction + poly + −2^(−M) correction) |
| FTWOTOX | 0x11 | N=round(64X), r=X−N/64, R=r×ln(2), shared poly with EXPTBL, scale |
| FTENTOX | 0x12 | N=round(X×64×log₁₀/log₂) with Cody-Waite, shared FTWOTOX poly, scale |
| FATAN | 0x0A | Four ranges: tiny (→X), small (single poly), main (table: F + U=(X−F)/(1+XF) + poly), large (−1/X + poly) |
| FSIN | 0x0E | Argument reduction mod π/2, degree-7 sin or degree-8 cos poly selected by quadrant |
| FCOS | 0x1D | Same reduction as FSIN, quadrant-shifted poly selection |
| FSINCOS | 0x30–0x37 | Returns sin; stores cos in FP[cos_reg]. Interleaved Horner for both polys |
| FTAN | 0x0F | Delegates to sin/cos division |
| FSINH | 0x02 | Large: exp(|X|−16381×ln2)×2^16380; normal: z=expm1(|X|), (z+z/(1+z))/2 |
| FCOSH | 0x19 | Large: same as FSINH; normal: z=exp(|X|), (z/2)+(1/(2z)) |
| FTANH | 0x09 | Bounds-based: tiny (→X), standard (expm1-based), large (→±1) |
| FASIN | 0x0C | atan(X / sqrt((1−X)(1+X))); |X|>1 → OPERR |
| FACOS | 0x1C | 2×atan(sqrt((1−X)/(1+X))); special cases for X=±1 |
| FATANH | 0x0D | (1/2)×lognp1(2|X|/(1−|X|)); |X|≥1 → DZ or OPERR |

### Argument Reduction

- **Trigonometric:** Two-path reduction. Fast path uses the 65-entry PITBL for |X| ≤ 15π. General path (`trig_reduce_general`) uses iterative Cody-Waite reduction for larger arguments. Result is (r, N) where r ∈ [−π/4, π/4] and N mod 4 selects the polynomial and sign.
- **Exponential:** Cody-Waite with split log₂ constant (lead + trail) for cancellation accuracy.
- **Logarithmic:** Table-driven with 64 precomputed (1/F, log(F)) pairs. The top mantissa bits select F, then U=(Y−F)×(1/F) is fed to a short polynomial.

## Condition Code Evaluation

The FPSR condition code bits (N, Z, I, NaN in bits 27:24) are updated after each arithmetic operation and comparison. The `fpu_test_condition()` function evaluates all 32 IEEE-aware condition predicates used by FBcc, FScc, FDBcc, and FTRAPcc. Predicates 0x10–0x1F set the BSUN exception if the NaN condition code bit is set.

## Rounding

The `fpu_pack()` function applies rounding when converting from the unpacked 128-bit format back to `float80_reg_t`:

- **Rounding mode** (FPCR bits 5:4): round-to-nearest-even (default), round-toward-zero, round-toward-−∞, round-toward-+∞
- **Rounding precision** (FPCR bits 7:6): extended (64-bit mantissa), single (24-bit), double (52-bit)

For single and double precision, the mantissa is rounded to the target width and excess bits are discarded. This matches the 68882 behavior where precision control affects the result stored in the FP register.

The transcendental module also uses `fp_rnd64()` to round intermediate results to 64-bit mantissa precision after each step, faithfully simulating the 68882's internal pipeline rounding behavior.
