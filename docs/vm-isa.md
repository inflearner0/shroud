# Shroud VM bytecode format

Status: implemented for the scalar-integer subset. Notes below describe the
current format; future work (superoperators, per-block re-keying, dispatch
hardening) is marked as planned.

## Machine model

- 64-bit register machine with a 256-entry register file.
- Register 0 is the first function argument and the default return value.
- Layout: values `0..223`, temporaries `224..238`, void-call result sink
  `239`, call argument slots `240..247`.
- No flags: comparisons produce 0/1 values, matching LLVM IR semantics.
- Cells exist for `i1`, `i8`, `i16`, `i32`, `i64`, and `ptr`. Every value is
  stored zero-extended; signed operations `SEXT` into temporaries first.
  Floating point and vectors are not virtualized yet.

## Instruction encoding

Every instruction is one 8-byte cell:

| Bits | Field |
| --- | --- |
| 0-7 | opcode |
| 8-15 | destination register |
| 16-23 | source A register |
| 24-31 | source B register |
| 32-63 | immediate / branch target / register index |

`CONST64` occupies two cells: the low half in the first cell, the high half in
the second. `b` is not currently used as a sentinel; all sources are registers
and constants are materialized in a prologue.

## Opcodes

| Range | Class | Implemented ops |
| --- | --- | --- |
| 0x00-0x1F | data | `HALT`, `CONST`, `CONST64`, `MOV` |
| 0x04-0x12 | arithmetic | `ADD`, `SUB`, `MUL`, `UDIV`, `SDIV`, `UREM`, `SREM`, `AND`, `OR`, `XOR`, `SHL`, `LSHR`, `ASHR`, `NEG`, `NOT` |
| 0x13-0x1C | compare/select | `ICMP_EQ/NE/ULT/ULE/UGT/UGE/SLT/SLE/SGT/SGE`, `SELECT` |
| 0x1E-0x21 | control | `JMP`, `CONDBR`, `SEXT`, `TRUNC` |
| 0x22-0x29 | memory | `LOAD8/16/32/64`, `STORE8/16/32/64` |
| 0x2A-0x2B | address | `GEP_IMM`, `GEP_REG` |
| 0x2C-0x2F | calls | `CALL`, `CALL_INDIRECT`, `RET`, `LOADADDR` |
| 0x30-0x35 | bulk/convert | `MEMCPY`, `MEMMOVE`, `MEMSET`, `BSWAP16/32/64` |
| 0xA0-0xBF | superoperators | planned |
| 0xC0-0xFF | pseudo-ops | planned (spill, reload, decrypt trap) |

Branch semantics: `CONDBR a, target` jumps when register `a` is non-zero,
otherwise falls through. Conditional branches and switches compile to a
fallthrough chain plus out-of-line stubs holding the successor's PHI copies.

## Encryption

- Key: `K = mixSeed(moduleSeed, "vm:" + functionName)`.
- Keystream: successive outputs of splitmix64 seeded with `K`; each cell is
  XORed with one output.
- The wrapper decrypts into a stack buffer at function entry. Static analysis
  sees no plaintext opcodes; the decrypted buffer exists only at runtime.
- Planned: per-block subkeys, re-encryption during dispatch, and self-modifying
  handler stubs.

## Native boundary

- Direct calls: the lifter emits `CALL table_index` with arguments placed in
  registers `240..247`. Each callee gets a generated 8-argument thunk that
  converts registers to the real signature and zero-extends the result.
- Indirect calls: the callee pointer is in a register and the same eight
  argument slots are passed in the platform C ABI registers (`x86-64`, `arm64`).
  This is correct for integer/pointer signatures with up to eight arguments.
- Global addresses (globals, function pointers, string data) are loaded from a
  per-function `shroud.vm.globals` table via `LOADADDR`.
- Functions that cannot be virtualized (varargs, `setjmp`, FP, vectors, inline
  asm, >8 arguments) are skipped with a reason and keep the entry cloak.

## Correctness rules

- All integer arithmetic is two's complement wrapping; signed semantics come
  from explicit `SEXT`/`TRUNC`, never from `nsw`/`nuw`.
- PHI nodes become parallel edge copies; two-phase copies are used when
  sources and destinations overlap.
- Allocas are hoisted into the wrapper frame (entry block, constant size only);
  pointers are stored in registers and accessed natively.
