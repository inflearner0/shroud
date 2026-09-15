# Architecture

## Pipeline placement

Shroud is an out-of-tree LLVM pass plugin. It registers two entry points:

1. `OptimizerLastEP` runs after the whole `-O1`/`-O2`/... pipeline, so no LLVM
   pass can fold or delete the injected code afterwards. This is the normal
   mode used by `clang -fpass-plugin=`.
2. Pipeline parsing (`opt -passes=shroud`) for IR-level debugging.

```text
C source --clang--> LLVM IR --optimizer pipeline--> [ shroud at OptimizerLast ] --> object
```

## Components

| Component | Files | Responsibility |
| --- | --- | --- |
| RNG | `include/shroud/RNG.h`, `src/RNG.cpp` | xoshiro256** and seed derivation (`mixSeed`) |
| Config | `include/shroud/Config.h`, `src/Config.cpp` | spec parsing, per-function `Plan`, module defaults |
| Annotation | `include/shroud/Annotation.h`, `src/Annotation.cpp` | reads `llvm.global.annotations` and `!shroud` metadata |
| Pass | `src/ShroudPass.cpp` | selection, per-function seeding, transforms |

## Invariants

- Deterministic: the same IR, seed, and annotations always produce the same
  output. Per-function RNGs are seeded with `mixSeed(moduleSeed, functionName)`.
- Semantics preserving: transformations must not alter observable behavior;
  the differential harness enforces this byte for byte.
- Last pass wins: transformations run at `OptimizerLast`; anything earlier is
  erased by InstCombine/GVN.
- No work without a `shroud` annotation; unannotated modules compile unchanged.

## Entry cloaking (current transform)
For each selected function:

1. A new entry block `shroud.entry` is created in front of the original entry.
2. A runtime predicate compares two module globals, `shroud.anchor` and
   `shroud.expected`, which are equal by construction: the `shroud.init`
   constructor registered in `llvm.global_ctors` copies the anchor value into
   `expected` before `main` runs. The pass runs last, so no LLVM pass can see
   through the two independent loads, and SelectionDAG cannot constant-fold
   the branch.
3. The predicate branches to the real entry (taken) or to `shroud.junk`
   (never taken). The junk block runs a seeded arithmetic chain built from
   seed-dependent constants and jumps to the real entry. Different seeds
   change the junk constants, the predicate form, and the anchor value, so
   each build is unique.

Adding a predecessor to the original entry is safe because entry blocks cannot
contain PHI nodes.

## Virtual machine (current transform)

`virtualize=1` lifts the function body into bytecode and replaces the body with
a wrapper. Components:

| Component | Files | Responsibility |
| --- | --- | --- |
| ISA | `include/shroud/VMBytecode.h` | opcode enum, register file layout |
| Runtime | `runtime/vm_runtime.c` | `shroud_vm_run`, `shroud_vm_decrypt` |
| Lifter | `src/VMVirtualizer.cpp` | IR to bytecode, tables, thunks, wrapper |
| Embedding | `cmake/EmbedBinary.cmake`, `include/shroud/VMRuntimeData.h` | runtime compiled to bitcode and embedded in the plugin |

Lifting:

1. Pre-scan assigns virtual registers to arguments, allocas and instruction
   results, validates every instruction, and pre-analyzes GEPs.
2. Constants are materialized in a prologue; 64-bit constants use `CONST64`,
   global addresses use a per-function global table (`LOADADDR`).
3. Each basic block is emitted sequentially and branches are patched to
   absolute cell indices. Conditional branches and switches use out-of-line
   stubs that perform the successor's PHI edge copies, so copies only run on
   the edge that needs them.
4. Integer widths are honored with explicit `SEXT`/`TRUNC` around signed
   operations and narrow results, so the 64-bit VM matches LLVM's two's
   complement semantics.

Runtime layout:

- 256 virtual registers: values `0..223`, temporaries `224..238`, void-call
  sink `239`, call arguments `240..247`. Register 0 is the first argument and
  the default return value.
- The wrapper allocates the register file and a decrypted bytecode buffer,
  stores arguments (zero-extended) and alloca addresses, decrypts the code
  with the per-function key, then calls `shroud_vm_run`.
- Direct calls go through generated 8-argument thunks in `shroud.vm.calls`;
  indirect calls use the C ABI registers directly.
- Calls between virtualized functions reach the callee's wrapper, so recursion
  and mutual recursion work unchanged.

Determinism: bytecode is a pure function of the IR; the key is
`mixSeed(moduleSeed, "vm:" + functionName)`, so seed changes alter only the
encryption and fallback cloaking.

Known restrictions (logged skip reasons, falls back to entry cloaking):
floating point, vectors, varargs, `setjmp`, inline asm, landingpads, calls with
more than eight arguments, dynamic or non-entry allocas, and functions with
unsupported intrinsics.

## Transform pipeline and runtime support

`src/Transforms.cpp` holds the source-level layers:

- MBA identities plus comparison rewrites and constant splitting with a
  runtime-opaque zero (the XOR of `shroud.anchor` and `shroud.expected`, which
  is zero unless anti-tamper fires).
- Branch laundering and decoy diamonds.
- Flattening with integer or pointer dispatch states; PHI nodes are lowered to
  per-phi slots updated on every edge.

`src/RuntimeSupport.cpp` owns every module-level global and the single
`shroud.init` constructor:

| Global | Purpose |
| --- | --- |
| `shroud.anchor` / `shroud.expected` | runtime-opaque equality used by cloaks, branch predicates, flattening, and constant splitting |
| `shroud.tamper` | set by debugger detection or integrity failure; VM wrappers trap while non-zero |
| `shroud.ready` | magic written by `shroud.init`; wrappers trap if the constructor never ran |
| `shroud.decoy` | volatile junk store target for decoy diamonds |

`shroud.init` decrypts strings/constant arrays, detects debuggers, verifies
every encrypted bytecode array with FNV, writes `shroud.tamper`, fixes
`shroud.expected`, and writes `shroud.ready`. It is emitted once per module,
after all functions have been processed, so integrity checks cover every VM
function regardless of processing order.

Determinism: functions are processed in name order, all randomness comes from
`mixSeed(moduleSeed, ...)`, and the runtime picks platform behavior from the
module triple, so the same seed and inputs always produce the same binary.
