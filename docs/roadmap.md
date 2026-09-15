# Roadmap

A phase is only complete when `ctest` is green on Linux and the technique is
exercised by the differential target.

## Phase 1 - foundation (done)

- [x] LLVM 18 out-of-tree pass plugin
- [x] annotation and spec parsing (`shroud;key=value;...`)
- [x] deterministic per-function RNG
- [x] opaque predicate and bogus block entry cloaking
- [x] differential harness (baseline vs obfuscated, byte for byte)
- [x] unit tests for RNG and Config
- [x] CI (Linux full test suite, Windows build plus unit tests)

## Phase 2 - opaque predicates and bogus control flow

- [x] predicate library: parity, XOR, OR-complement, unsigned wrap forms
- [x] inject opaque predicates at function entries (cloaking)
- [x] inject predicates at individual conditional branches (`bpred`)
- [x] block splitting and diamond decoys with junk arithmetic
- [ ] MBA-obfuscate the opaque predicate operands themselves

## Phase 3 - MBA (mixed boolean-arithmetic)

- [x] rewrite table for `add`, `sub`, `xor`, `and`, `or`
- [x] configurable layering depth (randomized per instruction, capped at 3)
- [x] wrapping semantics only (no UB, no nsw/nuw flags)
- [x] constant splitting (`c -> (c^k) ^ (k^zero)` with runtime-opaque zero)
- [x] comparisons (`slt/sle/sgt/sge -> sub vs 0`, `eq/ne -> xor vs 0`, unsigned swaps)
- [ ] multiplies

## Phase 4 - control-flow flattening

- [x] dispatcher loop with a random state variable
- [x] PHI-to-memory conversion and state updates on every edge
- [x] opaque state transitions (offsets by the runtime-opaque zero)
- [x] two dispatcher styles: integer ids and pointer states

## Phase 5 - VM

- [x] bytecode ISA (see `vm-isa.md`)
- [x] lifter: IR to bytecode for annotated functions
- [x] interpreter with per-function encrypted bytecode
- [x] typed thunks for native calls, indirect calls, global address tables
- [x] PHI edge copies via branch stubs, switch lowering, memory ops, GEP
- [x] bytecode emulator for debugging (`tools/vmtrace.py`)
- [x] superoperators (`GEP+LOAD/STORE` fusion)
- [x] handler duplication (alternate opcode implementations, MBA-derived)
- [x] per-block key chaining
- [x] runtime bytecode re-verification during dispatch
- [ ] self-modifying dispatch (decode-on-execute chunks)

## Phase 6 - data

- [x] string literal encryption with global-constructor decryption
- [x] constant array encryption (any integer element type), startup decrypt
- [ ] indirect call tables (native call table obfuscation)

## Phase 7 - anti-tamper (opt-in)

- [x] bytecode integrity checks (FNV over every encrypted code array)
- [x] debugger detection (`ptrace` / `IsDebuggerPresent`) feeding a tamper trap
- [x] `SHROUD_NO_TAMPER=1` escape hatch for debugging protected builds
- [x] runtime re-verification of executable bytecode (init + periodic)
- [x] `shroud.ready` guard: VM traps if the init constructor was removed
- [ ] PE/ELF section header/page integrity checks (substituted for now by
      bytecode and init-run integrity, which are portable)

## Phase 8 - polish

- [x] `shroudc` driver with profile files (`SHROUD_PROFILE`, `--profile=`)
- [x] per-transform statistics in `SHROUD_VERBOSE`
- [x] benchmarks and overhead budget documented in the README
- [x] contributor docs for adding transforms (`docs/contributing.md`)
