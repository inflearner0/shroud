# Shroud

LLVM IR obfuscator with a planned register-based VM back end. Shroud runs as an
out-of-tree LLVM pass plugin and injects transformations at the very end of the
optimization pipeline, so later LLVM passes cannot fold or delete them.

Status: the core protection stack is implemented and tested on Linux and
Windows: opaque-predicate entry cloaking, MBA expression rewriting,
control-flow flattening, string encryption, an encrypted register VM, and
basic anti-tamper (debugger detection plus bytecode integrity checks feeding a
tamper trap). The differential suite covers a 27-section C testbed plus a
crackme, all byte-for-byte identical between baseline and obfuscated builds.
Remaining roadmap items are listed in [docs/roadmap.md](docs/roadmap.md).

## Layout

| Path | Contents |
| --- | --- |
| `include/shroud` | public headers: `RNG`, `Config`, `Annotation` |
| `src` | pass plugin and support libraries |
| `tests/programs` | differential test programs |
| `tests/unit` | dependency-free unit tests |
| `tests/differential.py` | baseline-vs-obfuscated harness |
| `tools/shroudc.py` | clang wrapper that injects the plugin |
| `docs` | architecture, VM ISA draft, roadmap |

## Requirements

- LLVM 18 development package (tested; newer LLVM majors change the pass-plugin
  APIs and are rejected with a warning), CMake >= 3.20, a C++17 compiler
- Python 3 for the differential test

Ubuntu:

```bash
sudo apt install clang-18 llvm-18-dev cmake ninja-build python3
```

## Build

```bash
cmake -S . -B build -G Ninja -DLLVM_DIR=/usr/lib/llvm-18/lib/cmake/llvm
cmake --build build
ctest --test-dir build --output-on-failure
```

Windows (MSVC plus the official LLVM 18 installer):

```powershell
cmake -S . -B build -DLLVM_DIR="C:/Program Files/LLVM/lib/cmake/llvm"
cmake --build build --config Release
ctest --test-dir build -C Release -R unit --output-on-failure
```

Pass plugins on Windows require an LLVM built with
`LLVM_EXPORT_SYMBOLS_FOR_PLUGINS`. Official LLVM Windows installers enable
this; plain builds may not load the DLL.

## Usage

```bash
clang-18 -O2 -fpass-plugin=$PWD/build/src/shroud.so target.c -o target
# with a profile instead of annotations:
SHROUD_PROFILE=shroud.profile clang-18 -O2 -fpass-plugin=... target.c -o target
```

Through the wrapper (`python3 tools/shroudc.py --plugin=... --profile=... -O2 ...`).

Select functions in C with the `annotate` attribute:

```c
#if defined(__clang__)
#define SHROUD(spec) __attribute__((annotate(spec)))
#else
#define SHROUD(spec)
#endif

SHROUD("shroud;mba=2;virtualize=1")
static int license_check(const char *key) { ... }
```

### Spec grammar

`shroud` enables the pass for the function. Options are `;`-separated
`key=value` pairs, applied left to right, each spec inheriting the previous
one. Module-wide defaults can be set with named metadata
`!shroud = !{!"opaque=1;mba=1"}`.

| Key | Values | Default | Status |
| --- | --- | --- | --- |
| `opaque` | bool | on | implemented (entry cloaking) |
| `bogus` | bool | on | implemented (junk block) |
| `bpred` | bool | off | implemented (branch laundering + decoy diamonds) |
| `mba` | depth 1-3 | 0 | implemented (expressions, comparisons, constant splitting) |
| `flatten` | bool | off | implemented (integer/pointer dispatcher, opaque transitions) |
| `virtualize` | bool | off | implemented (VM) |
| `strings` | bool | off | implemented (keystream XOR + startup decrypt) |
| `consts` | bool | off | implemented (constant arrays, startup decrypt) |
| `seed` | u64 | module seed | implemented |

### Environment

| Variable | Meaning |
| --- | --- |
| `SHROUD_SEED` | module seed (decimal or `0x...`), default `0x243F6A8885A308D3` |
| `SHROUD_VERBOSE` | `1` prints the per-function plan and a summary to stderr |
| `SHROUD_DUMP` | `1` dumps lifted bytecode (ops, registers, basic blocks) to stderr |
| `SHROUD_NO_TAMPER` | `1` disables debugger detection and integrity checks (for debugging protected builds) |
| `SHROUD_PROFILE` | path to a profile file (`glob = shroud;spec` lines) applied without annotations |

## Transform layers

Transforms apply in this order, each driven by the annotation spec or profile:

1. **Data encryption** (`strings=1`, `consts=1`) — referenced string literals
   and constant arrays are re-encrypted in place with a per-global xorshift64
   keystream and moved to writable data; `shroud.init` decrypts them before
   `main`.
2. **MBA** (`mba=N`) — integer `add`/`sub`/`xor`/`and`/`or` expressions are
   rewritten into layered identities (e.g. `a+b -> (a^b) + 2*(a&b)`), integer
   comparisons are rewritten (`slt a,b -> slt (a-b), 0`, `eq -> eq (a^b), 0`,
   unsigned predicates swapped), and constants are split with a runtime-opaque
   zero (`c -> (c^k) ^ (k^zero)`). Random per instruction with an expansion
   budget.
3. **Branch predicates** (`bpred=1`) — every conditional branch condition is
   laundered through an opaque-true value, and roughly half the edges gain a
   decoy diamond block with junk arithmetic and a volatile store.
4. **Control-flow flattening** (`flatten=1`) — every basic block becomes a case
   of a dispatcher loop; the state variable is either integers (random ids) or
   pointers to per-block globals, transitions are offset by the runtime-opaque
   zero, and PHI nodes are converted to memory edges.
5. **VM virtualize** (`virtualize=1`) — the (already transformed) body is
   lifted to bytecode; unsupported functions fall back to entry cloaking.
6. **Entry cloak** — applied unless the function was virtualized.
7. **Anti-tamper** — `shroud.init` runs debugger detection (`ptrace` on Linux,
   `IsDebuggerPresent` on Windows) and FNV integrity checks over every
   encrypted bytecode array, then marks `shroud.ready`. Any mismatch sets
   `shroud.tamper`; VM wrappers trap (`__builtin_trap`) when the module is
   tampered with or when `shroud.init` has not run, and the interpreter
   re-verifies the encrypted bytecode every 4096 instructions.

## VM hardening

- Bytecode is encrypted per block: each block's keystream is chained from the
  previous block's key, so partial dumps do not decrypt in isolation.
- Superoperators fuse `GEP + LOAD/STORE` pairs into single instructions.
- Common opcodes have alternate handlers (`ADD_ALT` via the MBA identity,
  `XOR_ALT`, `MOV_ALT`, `LOAD32_ALT` via volatile, `ICMP_EQ_ALT`) that are
  selected randomly per site, so opcode histograms differ between builds.
- The interpreter verifies the encrypted bytecode periodically and traps on
  mismatch.

## Performance

Measured with `build/gen_bench.py` (120 virtualized kernels, 10 workload types,
`-O2 -fno-vectorize -fno-slp-vectorize`):

| Platform | Baseline | Virtualized | Ratio |
| --- | ---: | ---: | ---: |
| Linux x86-64 | 507 ms | 11.9 s | 23.6x |
| Windows x86-64 | 477 ms | 13.0 s | 27.4x |

Per-workload overhead ranges from ~11x (dense ALU loops) to ~86x (memory-heavy
matmul); call-heavy recursion pays the wrapper cost per call. All measurements
include the MBA/flatten/anti-tamper layers on the VM'd functions.

## The VM

With `virtualize=1`, an annotated function's body is lifted into bytecode and
replaced by a wrapper:

```text
wrapper:
  regs[256] = { arguments, alloca addresses, ... }
  shroud_vm_decrypt(encrypted_code, stack_buffer, key)
  return shroud_vm_run(stack_buffer, regs, call_table, global_table)
```

- Register-based ISA, 8-byte cells, ~54 opcodes (ALU, shifts, comparisons,
  select, branches, memory, GEP, direct/indirect calls, memcpy/memset, bswap).
- Per-function key derived from `SHROUD_SEED`; the bytecode is XOR-encrypted
  with a splitmix64 keystream and decrypted at runtime into a stack buffer.
- Calls to native functions go through typed thunks; calls to other
  virtualized functions reach their wrappers transparently. Indirect calls
  pass integer/pointer arguments in the platform C ABI registers (x86-64,
  arm64).
- `PHI` nodes become edge copies in branch stubs, so both paths through a
  conditional branch keep correct SSA semantics.
- Unsupported constructs are skipped with a logged reason and fall back to
  entry cloaking: floating point, vector types (`<4 x i32>` etc.), varargs,
  `setjmp`/`longjmp` users, inline asm, >8 call arguments, exceptions.
- `tests/programs/obf_target.c` virtualizes 25 functions under
  `-fno-vectorize -fno-slp-vectorize` and 23 with default `-O2`.

`tools/vmtrace.py` is a bytecode emulator that parses the `SHROUD_DUMP` output
and executes it, which makes miscompile debugging a matter of tracing.

## Testing

`tests/differential.py` builds each program twice (with and without the
plugin), runs both, and compares stdout and exit status byte for byte. It also
checks that a fixed seed is deterministic and that changing `SHROUD_SEED`
changes the binary. `tests/programs/obf_target.c` is a 27-section
self-checking C program covering phi nodes, recursion, varargs, `setjmp`,
function pointers, FP math, bitfields, and heap data; it prints a final digest
that must match exactly. `ctest` runs it twice: default `-O2` and a scalar
variant (`-fno-vectorize -fno-slp-vectorize`) that exercises the VM on
`matmul` and `memmove_test` as well. `tests/programs/crackme.c` is a staged
key-check demo; its test verifies grant/deny behavior for seven inputs, string
encryption (plaintext absent from the obfuscated binary) and, when `gdb` is
available, that the correct key is denied while running under a debugger.

## Roadmap

See [docs/roadmap.md](docs/roadmap.md), [docs/architecture.md](docs/architecture.md),
and the VM bytecode draft in [docs/vm-isa.md](docs/vm-isa.md).

## License

MIT
