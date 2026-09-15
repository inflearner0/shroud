# Contributing

## Layout

| Path | Contents |
| --- | --- |
| `include/shroud/Config.h`, `src/Config.cpp` | annotation spec parsing, plans, glob matcher (LLVM-free, unit-tested) |
| `include/shroud/RNG.h`, `src/RNG.cpp` | deterministic RNG and seed derivation (LLVM-free, unit-tested) |
| `include/shroud/Annotation.h`, `src/Annotation.cpp` | `llvm.global.annotations` and `!shroud` metadata readers |
| `include/shroud/RuntimeSupport.h`, `src/RuntimeSupport.cpp` | anchor/tamper/ready/decoy globals, string and constant encryption, the `shroud.init` constructor, bytecode checksums |
| `include/shroud/Transforms.h`, `src/Transforms.cpp` | MBA, constant splitting, branch predicates, flattening |
| `include/shroud/VMBytecode.h` | VM ISA shared by the lifter and the C runtime |
| `src/VMVirtualizer.cpp` | IR to bytecode lifter, tables, thunks, wrapper generation |
| `runtime/vm_runtime.c` | interpreter, decryptors, trap; compiled to bitcode at build time |
| `tools/vmtrace.py` | bytecode emulator for debugging lifted functions |
| `tests/` | differential harness, crackme harness, unit tests, programs |

## Adding a transform

1. Implement it in `src/Transforms.cpp` (or a new file added to `src/CMakeLists.txt`)
   with a declaration in `include/shroud/Transforms.h`.
2. Add a plan key in `Config.{h,cpp}` so it can be enabled per function via
   `SHROUD("shroud;<key>=1")`; document it in the README table.
3. Call it from `ShroudPass::run` in the documented order (data, MBA, branch
   predicates, flatten, VM, cloak). Anything that relies on the
   `anchor == expected` invariant must call `noteCloakUsed(M)` so
   `shroud.init` fixes the globals up at startup.
4. Use the per-function RNG (`shroud::mixSeed(moduleSeed, ...)`) for all
   randomness, never `rand()` or address ordering. Function processing order is
   sorted by name; keep it that way for reproducible builds.
5. Never leave a function semantically different: transformations must be
   exact for all inputs, using wrapping integer semantics only.
6. Exercise it in `tests/programs/obf_target.c` (or add a new program plus a
   ctest entry) so the differential harness compares baseline and obfuscated
   output byte for byte.
7. Run `cmake --build build && ctest --test-dir build --output-on-failure`.
   The suite includes determinism (same seed = same binary) and seed-variance
   (different seed = different binary) checks; both must pass.

## Debugging a miscompile

- Build the program with `SHROUD_DUMP=1` and inspect the lifted bytecode.
- Feed the dump into `python3 tools/vmtrace.py <dump>` to emulate execution.
- For suspected transform bugs, disable layers with the spec keys
  (`virtualize=0`, `mba=0`, ...) to bisect.
- Use `SHROUD_NO_TAMPER=1` to run protected builds under a debugger.
