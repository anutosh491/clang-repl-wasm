# Swift REPL in Browser — Project Summary

## Goal

Compile Swift's `SwiftImmediate` library (which contains the Swift REPL/Interpreter) to
WebAssembly via Emscripten, so the Swift REPL can run in a browser — directly analogous to
the `clang-repl-wasm` project.

---

## Background & Motivation

anutosh491 built a Swift REPL (PR #1 on `anutosh491/swift`) inspired by their prior work on
`clang-repl`. The implementation lives under `lib/Immediate/` (the `SwiftImmediate` library).
Max Desiatov has been driving Emscripten target support for Swift's stdlib
(`swiftlang/swift#87797`, `maxd/emscripten-sysroot` branch). Combining these two gives us
Swift REPL in the browser.

---

## Repository Layout

Everything lives under `/Users/anutosh491/work/swift-dev/`:

```
swift-dev/
  swift/                  ← anutosh491's fork (branch: emscripten-repl)
  llvm-project/           ← shallow clone of swiftlang/llvm-project
  emscriptenswift.py      ← Thorsten's new build product (see below)
  summary.md              ← this file
```

All other Swift dependencies (cmark, swift-driver, etc.) are pulled into `swift-dev/` by
`update-checkout`.

---

## The Working Branch: `emscripten-repl`

**Base:** Max Desiatov's `maxd/emscripten-sysroot` (rebased on swiftlang/swift main)  
**On top:** 13 REPL commits cherry-picked from `anutosh491/swift:repl`

The 13 commits (in order):
1. Reintroduce REPL entry point with basic read-eval loop skeleton
2. Introduce new `SourceFileKind::REPL`
3. Implement `wrapTopLevelCodeInFunction` AST transform (`REPLTransforms.cpp`)
4. SIL lowering step
5. IRGen and JIT step
6. `makeDeclarationsPublic` transform
7. Expose the Swift Interpreter (`Interpreter.h/cpp`)
8. Fix error recovery
9. `isInputComplete` and code completion support
10. Add last value printing support
11. Last value printing support for rich display
12. Make sure to `initializeSwiftModules` once per process
13. Enable REPL to respect optimization flags

**How to get it:**
```bash
cd swift-dev/swift
git remote add max git@github.com:MaxDesiatov/swift.git
git fetch max maxd/emscripten-sysroot --no-tags
git checkout -b emscripten-repl max/maxd/emscripten-sysroot
git fetch origin repl:refs/remotes/origin/repl --no-tags
# Apply all 13 commits as one diff (avoids REPL.cpp conflicts)
git diff $(git merge-base origin/repl origin/main) origin/repl | git apply --3way
git add -A && git commit -m "Add ORC JIT based REPL (Interpreter)"
```

> **Why `git apply` instead of `git cherry-pick`?**  
> anutosh491's commits were built incrementally on top of `REPL.cpp`, which Max already
> deleted. `cherry-pick` hits the same `REPL.cpp` delete/modify conflict on every commit.
> Applying the final net diff sidesteps all intermediate conflicts and lands the end state
> directly.

---

## Key Source File: `lib/Immediate/Interpreter.cpp`

This is the Swift REPL implementation. The pipeline per cell:

```
typeCheckREPLInput()       ← creates a new ModuleDecl, runs import resolution + type checking
      ↓
wrapTopLevelCodeInFunction()  ← AST transform: top-level stmts → __repl_N()
      ↓
makeDeclarationsPublic()   ← AST transform: raise access levels for JIT visibility
      ↓
autolinkImportedModules()  ← dlopen any libs introduced by this cell's imports
      ↓
performASTLowering()       ← Swift AST → SIL
      ↓
runSILDiagnosticPasses / runSILOptimizationPasses / runSILLoweringPasses
      ↓
performIRGeneration()      ← SIL → LLVM IR
      ↓
JIT->addIRModule() + JIT->lookup() + Fn()   ← ORC LLJIT execute
```

Other features already implemented:
- `isInputComplete()` — uses `ide::isSourceInputComplete()` to detect incomplete input
- `complete()` — uses `REPLCompletions` (Swift IDE) to return completion matches
- Auto-print: bare expressions → wrapped with `Swift.print(...)` or custom `DisplayFunc`
- `loadLibrary()` — `dlopen` wrapper for external `.so`/`.dylib`

---

## The Critical Architectural Challenge: No JIT in the Browser

`Interpreter.cpp` calls `SwiftJIT::Create(CI)` which creates an ORC LLJIT session. **This
doesn't exist in a wasm context.** There is no native JIT in the browser.

**The fix** (same approach as `clang-repl-wasm`):

Replace the JIT execution path with the `wasm-ld` side-module pattern:

```
For each REPL cell:
  IRGen → emit .wasm object  (via WebAssemblyTargetMachine)
         ↓
  wasm-ld → link as SIDE_MODULE .wasm
         ↓
  JS: WebAssembly.instantiate() the side module
         ↓
  Call __repl_N() in the instantiated module
```

This is exactly what `WasmIncrementalExecutor` does in clang-repl. A similar
`SwiftWasmExecutor` (or a wasm-specific path inside `Interpreter.cpp` guarded by
`#ifdef __EMSCRIPTEN__`) is needed.

**Also note:** The `#if !defined(__EMSCRIPTEN__)` guard must wrap:
- `dlopen` / `dlsym` calls (runtime library loading)
- `SwiftJIT::Create()` call
- `JIT->addIRModule()` and `JIT->lookup()` calls

---

## Existing Build Products

Located in `swift/utils/swift_build_support/swift_build_support/products/`:

### `emscriptensysroot.py` — `EmscriptenSysroot`
Builds the Emscripten C/C++ sysroot (libc, libc++, compiler-rt) using `embuilder.py`.
This is the foundation everything else builds against.
Install path: `<build-root>/emscripten-sysroot/wasm32-emscripten/sysroot/`

### `emscriptenstdlib.py` — `EmscriptenStdlib`
Cross-compiles the **Swift standard library only** for `wasm32-emscripten`.
- `SWIFT_INCLUDE_TOOLS=FALSE` — no compiler tools built
- Triggered by `--build-emscriptenstdlib` flag
- Sufficient for running Swift programs compiled ahead-of-time, but NOT for the REPL

### `emscriptenswift.py` — `EmscriptenSwift` (Thorsten's, in `swift-dev/` root)
New product that cross-compiles **Swift including tools** (`SWIFT_INCLUDE_TOOLS=TRUE`).
This is what's needed to build `swiftImmediate` for the browser.

Steps it performs:
1. Build `cmark` for wasm32 (Swift's Markdown dependency)
2. Configure + build LLVM for wasm32 (needed for Clang CMake paths)
3. Build Swift with `SWIFT_INCLUDE_TOOLS=TRUE`, pointing at the wasm LLVM and cmark

Extra dependencies it requires: `libuuid`, `libzstd` at `$WASM_PREFIX`.

**⚠️ Known issue — hardcoded path:**
```python
assert "/Users/thorstenbeier/micromamba/envs/swift-wasm/lib/cmake/cmark-gfm" == cmark_cmake_dir
```
This must be removed before the file is usable on any other machine.

---

## Key Differences: `EmscriptenStdlib` vs `EmscriptenSwift`

| | EmscriptenStdlib | EmscriptenSwift |
|---|---|---|
| `SWIFT_INCLUDE_TOOLS` | `FALSE` | `TRUE` |
| Builds cmark for wasm | No | Yes |
| Builds LLVM for wasm | Configure only | Configure + build |
| Requires `WASM_PREFIX` | No | Yes |
| Purpose | stdlib for AOT | full interpreter for browser |
| Status | In-tree, working | Out-of-tree, needs fixes |

---

## `SwiftImmediate` CMake Dependencies

```cmake
add_swift_host_library(swiftImmediate STATIC
  SwiftMaterializationUnit.cpp
  Immediate.cpp
  Interpreter.cpp
  REPLTransforms.cpp
  LLVM_LINK_COMPONENTS executionengine linker mcjit orcjit ...)
target_link_libraries(swiftImmediate PRIVATE
  swiftFrontend
  swiftIDE
  swiftIRGen
  swiftSILGen
  swiftSILOptimizer)
```

For the wasm build, `orcjit`/`mcjit`/`executionengine` LLVM components and the `swiftJIT`
dependency must be excluded or conditionally excluded under `#ifdef __EMSCRIPTEN__`.

---

## Analogies with clang-repl-wasm

| clang-repl-wasm | Swift REPL wasm |
|---|---|
| `clang::Interpreter` | `swift::Interpreter` |
| `WasmIncrementalExecutor` | To be written: `SwiftWasmExecutor` |
| `lldMain()` per cell | wasm-ld per cell |
| `CompilerModule.cpp` | Analogous Swift C++ glue module |
| `clangInterpreter` library | `swiftImmediate` library |
| `build/index.html` | New HTML frontend for Swift REPL |

---

## Build Steps: Compiling SwiftImmediate to wasm32

The approach mirrors how `clang-repl-wasm` was built: use emcmake/emcc to cross-compile
everything to wasm32, then link into a single wasm module. Done manually (no build-script).

### Environment setup

Stay in the `llvm` conda env (cmake is there) and activate emsdk on top of it:

```bash
source /Users/anutosh491/work/emsdk/emsdk_env.sh
# verify
emcc --version
```

Install ninja if not already present:
```bash
micromamba install ninja
```

**`WASM_PREFIX`** is the single unified prefix for **all** wasm32 dependencies: the
emscripten-forge packages (zstd, libuuid) already live there, and LLVM + cmark will be
installed there too. Swift's cmake points at it for everything.

```bash
# Create the emscripten-forge env once (if not done already)
micromamba create -f /Users/anutosh491/work/swift-dev/environment.yml \
  --platform=emscripten-wasm32 \
  -c https://prefix.dev/emscripten-forge-4x \
  -c https://prefix.dev/conda-forge

export WASM_PREFIX=/Users/anutosh491/micromamba/envs/swift-wasm
# verify zstd and libuuid are there
ls $WASM_PREFIX/lib/libzstd.a
ls $WASM_PREFIX/lib/libuuid.a
```

---

### Phase 0 — Build native tablegen tools

These are host executables (macOS arm64) that LLVM's cmake needs during the wasm32
cross-compilation to generate source files. Build them once from the Swift LLVM fork:

```bash
cd /Users/anutosh491/work/swift-dev/llvm-project
mkdir -p native_build && cd native_build

cmake ../llvm \
  -DLLVM_ENABLE_PROJECTS=clang \
  -DLLVM_TARGETS_TO_BUILD=host \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DLLVM_ENABLE_ZSTD=OFF \
  -DLLVM_ENABLE_ZLIB=OFF \
  -DLLVM_ENABLE_LIBXML2=OFF

make llvm-tblgen clang-tblgen -j$(sysctl -n hw.logicalcpu)
```

The binaries land at `native_build/bin/llvm-tblgen` and `native_build/bin/clang-tblgen`.
Set the path:

```bash
export NATIVE_LLVM_BIN=/Users/anutosh491/work/swift-dev/llvm-native-full-build/bin
```

---

### Phase 1 — Build Swift's LLVM fork for wasm32

Source: `swift-dev/llvm-project/llvm`. Build only the specific static libraries needed —
no install step. Swift's cmake can use the build directory directly because LLVMConfig.cmake,
ClangConfig.cmake, and LLDConfig.cmake are generated at cmake configure time (not build time).

**Exact LLVM components needed** (traced from swiftImmediate's full dep tree):
- swiftBasic: `support`, `targetparser`
- swiftLLVMPasses: `analysis`
- swiftIRGen: `target`, `transformutils`, `irprinter`
- swiftAST: `bitreader`, `bitwriter`, `core`, `coroutines`, `coverage`, `debuginfoDWARF`,
  `instrumentation`, `ipo`, `irreader`, `lto`, `mc`, `mcparser`, `object`, `objcarcopts`,
  `option`, `profiledata`, `remarks`, `WebAssembly` (target backend)
- swiftClangImporter/Serialization: `BitstreamReader`
- swiftImmediate (after #ifdef __EMSCRIPTEN__ patch): `transformutils` only (JIT removed)
- Future wasm execution: `lldWasm`, `lldCommon`
- ClangImporter requires full Clang frontend library set

```bash
mkdir -p /Users/anutosh491/work/swift-dev/llvm-wasm-build
cd /Users/anutosh491/work/swift-dev/llvm-wasm-build

emcmake cmake ../llvm-project/llvm \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_HOST_TRIPLE=wasm32-unknown-emscripten \
  -DLLVM_TARGETS_TO_BUILD="WebAssembly" \
  -DLLVM_ENABLE_PROJECTS="clang;lld" \
  -DLLVM_INCLUDE_BENCHMARKS=OFF \
  -DLLVM_INCLUDE_EXAMPLES=OFF \
  -DLLVM_INCLUDE_TESTS=OFF \
  -DLLVM_BUILD_TOOLS=OFF \
  -DLLVM_BUILD_UTILS=OFF \
  -DCLANG_BUILD_TOOLS=OFF \
  -DCLANG_ENABLE_STATIC_ANALYZER=OFF \
  -DCLANG_ENABLE_ARCMT=OFF \
  -DCLANG_ENABLE_BOOTSTRAP=OFF \
  -DLLVM_ENABLE_LIBEDIT=OFF \
  -DLLVM_ENABLE_LIBPFM=OFF \
  -DLLVM_ENABLE_THREADS=OFF \
  -DLLVM_ENABLE_ZSTD=OFF \
  -DLLVM_ENABLE_LIBXML2=OFF \
  -DLLVM_ENABLE_ZLIB=OFF \
  -DCMAKE_C_FLAGS="${EMCC_CFLAGS} -mtail-call" \
  -DCMAKE_CXX_FLAGS="${EMCC_CFLAGS} -mtail-call -Dwait4=__syscall_wait4" \
  -DLLVM_NATIVE_TOOL_DIR=$NATIVE_LLVM_BIN

# Build only the required static libs — no install
emmake make -j$(sysctl -n hw.logicalcpu) \
  LLVMSupport LLVMCore LLVMAnalysis LLVMTarget LLVMTargetParser \
  LLVMTransformUtils LLVMIRPrinter LLVMIRReader \
  LLVMBitReader LLVMBitWriter LLVMBitstreamReader \
  LLVMCoroutines LLVMCoverage LLVMDebugInfoDWARF \
  LLVMInstrumentation LLVMipo LLVMLTO LLVMMC LLVMMCParser \
  LLVMObject LLVMObjCARCOpts LLVMOption LLVMProfileData LLVMRemarks \
  LLVMWebAssemblyCodeGen LLVMWebAssemblyAsmParser \
  LLVMWebAssemblyDesc LLVMWebAssemblyInfo \
  lldCommon lldWasm \
  clangBasic clangLex clangParse clangAST clangASTMatchers \
  clangAnalysis clangEdit clangRewrite clangSerialization \
  clangFrontend clangDriver clangCodeGen
```

After build, verify cmake config files exist (generated at configure time):
```bash
ls /Users/anutosh491/work/swift-dev/llvm-wasm-build/lib/cmake/llvm    # must exist
ls /Users/anutosh491/work/swift-dev/llvm-wasm-build/lib/cmake/clang   # must exist
ls /Users/anutosh491/work/swift-dev/llvm-wasm-build/lib/cmake/lld     # must exist
```

Phase 4 uses the build dir directly (no install prefix for LLVM):
```
-DLLVM_DIR=/Users/anutosh491/work/swift-dev/llvm-wasm-build/lib/cmake/llvm
-DClang_DIR=/Users/anutosh491/work/swift-dev/llvm-wasm-build/lib/cmake/clang
```

---

### Phase 2 — Build cmark for wasm32

Swift's frontend links against cmark (GitHub Flavored Markdown parser).
Source: `swift-dev/cmark`. Installs into `$WASM_PREFIX`.

```bash
mkdir -p /Users/anutosh491/work/swift-dev/cmark-wasm-build
cd /Users/anutosh491/work/swift-dev/cmark-wasm-build

emcmake cmake ../cmark \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=$WASM_PREFIX \
  -DCMARK_TESTS=OFF \
  -DCMARK_SHARED=OFF \
  -DCMARK_STATIC=ON

emmake make -j$(sysctl -n hw.logicalcpu) install
```

Verify:
```bash
ls $WASM_PREFIX/lib/cmake/cmark-gfm   # must exist
```

---

### Phase 3 — Patch swiftImmediate CMakeLists.txt

Before attempting the Swift build, the JIT link components in
`swift/lib/Immediate/CMakeLists.txt` must be removed for wasm32 because those LLVM
components (`orcjit`, `mcjit`, `executionengine`, `orctargetprocess`, `jitlink`) are not
built into the wasm32 LLVM (no JIT in the browser). Also guard `Interpreter.cpp` JIT code
with `#ifdef __EMSCRIPTEN__`.

See the "Critical Architectural Challenge" section above — the wasm-ld execution path
must replace the ORC JIT path.

Minimal CMakeLists.txt change:
```cmake
# Remove these LLVM_LINK_COMPONENTS for the wasm32 build:
#   executionengine linker mcjit orcjit orctargetprocess jitlink transformutils
```

---

### Phase 4 — Build Swift compiler libs for wasm32

Source: `swift-dev/swift`. Builds only the compiler C++ libraries (no stdlib, no tools
executables). `swiftImmediate` and all its transitive C++ dependencies are the target.

`$WASM_PREFIX` serves triple duty here: cmake config files for LLVM/cmark (Phases 1–2),
libuuid and libzstd from emscripten-forge (for Swift's UUID and compression support).

```bash
rm -rf swift-wasm-build
mkdir -p /Users/anutosh491/work/swift-dev/swift-wasm-build
cd /Users/anutosh491/work/swift-dev/swift-wasm-build

emcmake cmake ../swift \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_SYSTEM_NAME=Emscripten \
  -DEMSCRIPTEN_SYSTEM_PROCESSOR=wasm32 \
  -DSWIFT_HOST_VARIANT_SDK=EMSCRIPTEN \
  -DSWIFT_HOST_VARIANT_ARCH=wasm32 \
  -DCMAKE_C_COMPILER_TARGET=wasm32-unknown-emscripten \
  -DCMAKE_CXX_COMPILER_TARGET=wasm32-unknown-emscripten \
  -DCMAKE_FIND_ROOT_PATH=$WASM_PREFIX \
  -DSWIFT_PATH_TO_CMARK_SOURCE=/Users/anutosh491/work/swift-dev/cmark \
  -DSWIFT_PATH_TO_CMARK_BUILD=/Users/anutosh491/work/swift-dev/cmark-wasm-build \
  -DSWIFT_CMARK_LIBRARY_DIR=/Users/anutosh491/work/swift-dev/cmark-wasm-build/src \
  -DLLVM_DIR=/Users/anutosh491/work/swift-dev/llvm-wasm-build/lib/cmake/llvm \
  -DLLD_DIR=/Users/anutosh491/work/swift-dev/llvm-wasm-build/lib/cmake/lld \
  -DClang_DIR=/Users/anutosh491/work/swift-dev/llvm-wasm-build/lib/cmake/clang \
  -DUUID_INCLUDE_DIR=$WASM_PREFIX/include \
  -DUUID_LIBRARY=$WASM_PREFIX/lib/libuuid.a \
  -Dzstd_INCLUDE_DIR=$WASM_PREFIX/include \
  -Dzstd_LIBRARY=$WASM_PREFIX/lib/libzstd.a \
  -DSWIFT_BUILD_RUNTIME_WITH_HOST_COMPILER=TRUE \
  -DSWIFT_NATIVE_SWIFT_TOOLS_PATH=$(dirname $(xcrun -f swift)) \
  -DSWIFT_NATIVE_CLANG_TOOLS_PATH=$(dirname $(xcrun -f clang)) \
  -DSWIFT_NATIVE_LLVM_TOOLS_PATH=$NATIVE_LLVM_BIN \
  -DLLVM_TABLEGEN=$NATIVE_LLVM_BIN/llvm-tblgen \
  -DCLANG_TABLEGEN=$NATIVE_LLVM_BIN/clang-tblgen \
  -DSWIFT_INCLUDE_TOOLS=TRUE \
  -DBOOTSTRAPPING_MODE=OFF \
  -DSWIFT_ENABLE_SWIFT_IN_SWIFT=OFF \
  -DSWIFT_INCLUDE_DOCS=FALSE \
  -DSWIFT_INCLUDE_TESTS=FALSE \
  -DSWIFT_BUILD_STATIC_STDLIB=FALSE \
  -DSWIFT_BUILD_DYNAMIC_STDLIB=FALSE \
  -DSWIFT_BUILD_SOURCEKIT=FALSE \
  -DSWIFT_TOOL_LIBSWIFTSCAN_BUILD=OFF \
  -DSWIFT_BUILD_REMOTE_MIRROR=FALSE \
  -DSWIFT_PRIMARY_VARIANT_SDK=EMSCRIPTEN \
  -DSWIFT_PRIMARY_VARIANT_ARCH=wasm32 \
  -DSWIFT_SDKS=EMSCRIPTEN \
  -DSWIFT_EMSCRIPTEN_SYSROOT_PATH=/Users/anutosh491/work/emsdk/upstream/emscripten/cache/sysroot \
  -DSWIFT_STDLIB_SINGLE_THREADED_CONCURRENCY=TRUE \
  -DSWIFT_ENABLE_DISPATCH=FALSE \
  -DSWIFT_THREADING_PACKAGE=none \
  -DCMAKE_C_COMPILER_WORKS=TRUE \
  -DCMAKE_CXX_COMPILER_WORKS=TRUE \
  -DCMAKE_Swift_COMPILER_WORKS=TRUE

emmake make -j$(sysctl -n hw.logicalcpu) swiftImmediate
```

---

### ✅ ACHIEVED: `swiftImmediate` builds as wasm32 static libs

The full pipeline from Phase 0–4 is complete. The build output at
`/Users/anutosh491/work/swift-dev/swift-wasm-build/lib/` contains:
- `libswiftImmediate.a` — the REPL engine
- `libswiftFrontend.a`, `libswiftIRGen.a`, `libswiftSIL.a`, `libswiftSILOptimizer.a` etc.
- `libswiftBasic.a`, `libswiftAST.a`, `libswiftSema.a`, `libswiftParse.a` etc.

---

## Pipeline Analysis: What Works for Wasm, What Doesn't

### The `parseAndExecute()` pipeline — step by step

**Step 1: `initializeSwiftModules()`** — called once in the constructor.  
With `SWIFT_ENABLE_SWIFT_IN_SWIFT=OFF`, `SwiftCompilerSources/stubs.cpp` provides an
empty no-op stub. This stub is an OBJECT library (`swiftCompilerStub`). It is NOT
automatically linked into `libswiftImmediate.a` — it is only linked into the driver binary
and `libSwiftScan`. **Action needed:** the final `CompilerModule.wasm` link must explicitly
include `swiftCompilerStub` (or pass `--allow-undefined` and never call the function, but
calling it as a no-op is cleaner).

**Step 2: `typeCheckREPLInput()` — parse + type-check (wasm ✅ with caveat)**  
Pure C++ AST work. BUT: it does `importInfo.StdlibKind = ImplicitStdlibKind::Stdlib`, which
tells the type checker to implicitly import `Swift` (the stdlib). **This requires `.swiftmodule`
files for the stdlib** at a path discoverable by `SearchPathOpts.RuntimeLibraryPaths`. Without
them, every cell fails with "cannot find module 'Swift'". The stdlib `.swiftmodule` files are
produced by the `EmscriptenStdlib` build product — **this must be built** before the REPL is
usable.

**Step 3: SIL lowering/optimization passes — (wasm ✅)**  
`performASTLowering`, `runSILDiagnosticPasses`, `runSILOptimizationPasses`,
`runSILLoweringPasses` — all pure C++ work, no JIT, no platform-specific code.
The `SWIFT_ENABLE_SWIFT_IN_SWIFT=OFF` stub means Swift-written optimizer passes
(LifetimeCompletion, etc.) are no-ops. The C++-only passes still run.

**Step 4: IRGen — (wasm ✅)**  
`performIRGeneration()` emits an LLVM Module targeting `wasm32-unknown-emscripten`. The
target triple is set from `IRGenOpts` which comes from the `CompilerInstance` configuration.
`performLLVMOptimizations()` follows. Both are pure C++.

**Step 5: `WasmExecutor::executeModule()` — (wasm ✅ design-wise, needs stdlib runtime)**  
Already implemented. The four sub-steps:

1. **Emit `.o`**: `llvm::legacy::PassManager` + `WebAssemblyTargetMachine` → wasm32 object.
   Works as long as `LLVMWebAssemblyCodeGen` is available (it is — linked in from our
   `llvm-wasm-build`).

2. **Link with `wasm-ld` (lld)**: calls `lld::wasm::link()` in-process. Uses:
   ```
   wasm-ld -shared --import-memory --stack-first --allow-undefined <cell.o> -o <cell.wasm>
   ```
   `--allow-undefined` lets Swift runtime symbols (`swift_retain`, `swift_release`,
   `swift_once`, `print()`, etc.) remain unresolved — they get resolved at dlopen time
   against the MAIN_MODULE. **This is correct.** The Swift runtime `.a` files must be
   baked into the MAIN_MODULE at initial link time (see Phase 5).

3. **`dlopen()` the side module**: Emscripten's `dlopen` is implemented via
   `WebAssembly.instantiate()` under the hood. It resolves all `--allow-undefined` symbols
   against the current module (the MAIN_MODULE). **Emscripten's `dlopen` works well in the
   browser** — confirmed by your testing.

4. **`dlsym()` + call**: the SIL-mangled wrapper name (e.g. `$s__repl_0AA3allyvF`) is the
   export. Wasm exports are raw strings, no ABI decoration. Should match exactly.

**The `autolinkImportedModules()` call** is already guarded `#ifndef __EMSCRIPTEN__` ✅ —
no `dlopen` of native `.dylib` files attempted.

---

### The `dlopen` Story for Wasm (Why This Works)

In Emscripten's browser runtime, `dlopen` is implemented using dynamic linking of wasm
modules. The MAIN_MODULE must be compiled with `-sDYNAMIC_EXECUTION=1` (or `-sMAIN_MODULE=1`)
and expose all Swift runtime symbols as exports. When a side module is `dlopen`'d:
- Emscripten calls `WebAssembly.instantiate()` with the side module's bytes
- Imports are resolved against the MAIN_MODULE's exports table
- The side module's exports become callable via `dlsym()`

This is functionally identical to the `clang-repl-wasm` approach. ✅

---

## What Needs to Be Built Next: The Swift Standard Library

**The stdlib serves two roles:**

| Role | What's needed | When |
|---|---|---|
| **Type-checking** | `.swiftmodule` files for `Swift`, `_Concurrency`, etc. | At `init()` time (warmup) |
| **Runtime symbols** | `libswiftCore.a` + `libswiftConcurrency.a` etc. | Baked into MAIN_MODULE |

The `EmscriptenStdlib` build product (`emscriptenstdlib.py`) builds exactly this.

### Why `emscriptensysroot.py` is NOT needed

The sysroot (Emscripten's C/C++ libc, libc++, compiler-rt) already exists at:
```
/Users/anutosh491/work/emsdk/upstream/emscripten/cache/sysroot
```
`emscriptensysroot.py` runs `embuilder` to build those and copy them to a build-local path.
Since the emsdk is already installed and active, there is nothing to build — the sysroot is
already there. Pass `SWIFT_EMSCRIPTEN_SYSROOT_PATH` pointing directly to the emsdk cache
sysroot in all cmake commands below.

### Why Xcode's swiftc cannot cross-compile the stdlib

The stdlib is written in Swift. Cross-compiling it to wasm32 requires a native `swiftc`
binary (runs on macOS ARM64) that accepts `-target wasm32-unknown-emscripten`. Apple's
released `swiftc` 6.3.2 does not support the `emscripten` OS environment — it lacks the
patches in `lib/Basic/Platform.cpp` and `cmake/modules/SwiftConfigureSDK.cmake` that Max
Desiatov added in `swiftlang/swift#87797`. We must build a native swiftc from
`swift-dev/swift` (which IS Max's branch, rebased).

That native swiftc also needs the **WebAssembly LLVM backend** compiled in, so it can
actually emit wasm32 object files when cross-compiling stdlib sources. The existing
`native_build` in `swift-dev/llvm-project/native_build/` was configured with
`LLVM_TARGETS_TO_BUILD=host` (AArch64 only) — we need to reconfigure it to also include
`WebAssembly`.

### Phase 5a — Build a separate native LLVM with WebAssembly backend

The existing `swift-dev/llvm-project/native_build/` was built with `LLVM_TARGETS_TO_BUILD=host`
(AArch64 only) solely for tablegen tools (`llvm-tblgen`, `clang-tblgen`) needed during Phase 1's
wasm32 LLVM cross-compile. **Do not touch it** — Phase 1 still references it via
`LLVM_NATIVE_TOOL_DIR`.

We need a NEW native LLVM build that also includes the **WebAssembly backend compiled into the
host LLVM static libs**. `LLVMWebAssemblyCodeGen.a` is a macOS ARM64 binary that *implements*
WebAssembly code generation — the native swiftc (Phase 5b) links against it to emit wasm32
object files.

```bash
mkdir -p /Users/anutosh491/work/swift-dev/llvm-native-full-build
cd /Users/anutosh491/work/swift-dev/llvm-native-full-build

cmake ../llvm-project/llvm \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=/usr/bin/clang \
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
  -DLLVM_ENABLE_PROJECTS="clang" \
  -DLLVM_TARGETS_TO_BUILD="AArch64;WebAssembly" \
  -DLLVM_ENABLE_ZLIB=OFF \
  -DLLVM_ENABLE_ZSTD=OFF \
  -DLLVM_ENABLE_LIBXML2=OFF \
  -DLLVM_INCLUDE_TESTS=OFF \
  -DLLVM_INCLUDE_BENCHMARKS=OFF \
  -DLLVM_INCLUDE_EXAMPLES=OFF \
  -DLLVM_BUILD_TOOLS=ON \
  -DLLVM_BUILD_UTILS=OFF \
  -DCLANG_BUILD_TOOLS=OFF

# Build LLVM+Clang static libs and llvm-ar/ranlib tools — ~30-45 min on M-chip Mac
make -j$(sysctl -n hw.logicalcpu)
```

Verify:
```bash
ls /Users/anutosh491/work/swift-dev/llvm-native-full-build/lib/libLLVMWebAssemblyCodeGen.a
ls /Users/anutosh491/work/swift-dev/llvm-native-full-build/bin/llvm-ar
ls /Users/anutosh491/work/swift-dev/llvm-native-full-build/lib/cmake/llvm/LLVMConfig.cmake
```

### Phase 5b — Build native swiftc from swift-dev/swift

This is the swiftc binary that will cross-compile stdlib `.swift` sources to wasm32. It
runs on macOS ARM64 but emits wasm32 object code (via the WebAssembly backend from Phase 5a).

First, build cmark for the native (macOS) host:
```bash
mkdir -p /Users/anutosh491/work/swift-dev/cmark-native-build
cd /Users/anutosh491/work/swift-dev/cmark-native-build

cmake ../cmark \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=/usr/bin/clang \
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
  -DCMARK_TESTS=OFF \
  -DCMARK_SHARED=OFF \
  -DCMARK_STATIC=ON

make -j$(sysctl -n hw.logicalcpu)
```

Then configure and build `swift-frontend`:
```bash
mkdir -p /Users/anutosh491/work/swift-dev/swift-native-build
cd /Users/anutosh491/work/swift-dev/swift-native-build

cmake -G Ninja ../swift \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=/usr/bin/clang \
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
  -DCMAKE_Swift_COMPILER=$(xcrun -f swiftc) \
  -DCMAKE_Swift_COMPILER_WORKS=TRUE \
  -DLLVM_DIR=/Users/anutosh491/work/swift-dev/llvm-native-full-build/lib/cmake/llvm \
  -DClang_DIR=/Users/anutosh491/work/swift-dev/llvm-native-full-build/lib/cmake/clang \
  -DSWIFT_PATH_TO_CMARK_SOURCE=/Users/anutosh491/work/swift-dev/cmark \
  -DSWIFT_PATH_TO_CMARK_BUILD=/Users/anutosh491/work/swift-dev/cmark-native-build \
  -DSWIFT_INCLUDE_TOOLS=TRUE \
  -DSWIFT_BUILD_RUNTIME_WITH_HOST_COMPILER=TRUE \
  -DSWIFT_ENABLE_SWIFT_IN_SWIFT=ON \
  -DBOOTSTRAPPING_MODE=HOSTTOOLS \
  -DSWIFT_INCLUDE_TESTS=OFF \
  -DSWIFT_INCLUDE_DOCS=OFF \
  -DSWIFT_BUILD_STATIC_STDLIB=OFF \
  -DSWIFT_BUILD_DYNAMIC_STDLIB=OFF \
  -DSWIFT_BUILD_SOURCEKIT=OFF \
  -DSWIFT_BUILD_REMOTE_MIRROR=OFF \
  -DSWIFT_TOOL_LIBSWIFTSCAN_BUILD=OFF \
  -DSWIFT_SDKS=OSX \
  -DSWIFT_PRIMARY_VARIANT_SDK=OSX \
  -DSWIFT_PRIMARY_VARIANT_ARCH=arm64 \
  -DSWIFT_EMSCRIPTEN_SYSROOT_PATH=/Users/anutosh491/work/emsdk/upstream/emscripten/cache/sysroot

# Build only swift-frontend — ~30-45 min on M-chip Mac
ninja -j$(sysctl -n hw.logicalcpu) swift-frontend
```

**What `BOOTSTRAPPING_MODE=HOSTTOOLS` means:**
cmake uses Xcode's swiftc (`CMAKE_Swift_COMPILER`) to compile any Swift-written parts of the
compiler itself (the "Swift-in-Swift" sources). `SWIFT_NATIVE_SWIFT_TOOLS_PATH` is intentionally
NOT set so cmake auto-sets it to `swift-native-build/bin` — this ensures that `swift-compatibility-symbols`
(built at step ~953) is found in the right place at step ~975, instead of looking for it in Xcode's
toolchain (which does not ship that tool). The NATIVE swiftc we're building will contain Max's wasm32
C++ patches and the WebAssembly LLVM backend from Phase 5a — making it capable of cross-compiling to
wasm32.

Verify:
```bash
ls /Users/anutosh491/work/swift-dev/swift-native-build/bin/swift-frontend
# cmake also creates a swiftc symlink:
ls /Users/anutosh491/work/swift-dev/swift-native-build/bin/swiftc
# If the symlink is missing, create it:
# ln -sf swift-frontend /Users/anutosh491/work/swift-dev/swift-native-build/bin/swiftc
```

### Phase 5b.5 — Build `swift-driver` (new driver, macOS host tool)

**Why this is needed:** Max's patches intentionally removed Emscripten support from the *legacy*
driver (`swiftc` acting as the old driver). The new driver (`swift-driver` package) has
`EmscriptenToolchain` and handles `wasm32-unknown-emscripten`. The legacy driver's runtime
lookup checks for `swift-driver` next to `swift-frontend`; if found it exec's into the new
driver; if not found it falls back to legacy mode → `unknown target` error.

**What to build:** `swift-driver` is a Swift Package (macOS host tool). Build it with Xcode's
swiftc. Do NOT use the custom `swift-native-build/bin/swiftc` here — that binary targets
the wasm stdlib build, not a macOS host tool.

**Do NOT delete or rebuild `swift-native-build`.** Just build this package and copy two binaries.

```bash
cd /Users/anutosh491/work/swift-dev/swift-driver

# Build using Xcode's swiftc (macOS host tool, not wasm)
# Package.resolved pins exact versions — SwiftPM fetches them automatically
/usr/bin/swift build -c release

# Drop the new driver binaries next to swift-frontend
cp .build/release/swift-driver /Users/anutosh491/work/swift-dev/swift-native-build/bin/swift-driver
cp .build/release/swift-help   /Users/anutosh491/work/swift-dev/swift-native-build/bin/swift-help
```

Verify the chain works — this must succeed (no "unknown target" error):
```bash
/Users/anutosh491/work/swift-dev/swift-native-build/bin/swiftc \
  -target wasm32-unknown-emscripten -print-target-info 2>&1 | head -5
```

### Phase 5c — Build Swift stdlib for wasm32

Uses the native swiftc from Phase 5b to cross-compile Swift source files to wasm32 objects.
`emcmake` sets `CMAKE_C_COMPILER=emcc` / `CMAKE_CXX_COMPILER=em++` for the C/C++ runtime
parts; the Swift sources are compiled by the native swiftc via `SWIFT_NATIVE_SWIFT_TOOLS_PATH`.

```bash
mkdir -p /Users/anutosh491/work/swift-dev/swift-stdlib-wasm-build
cd /Users/anutosh491/work/swift-dev/swift-stdlib-wasm-build

source /Users/anutosh491/work/emsdk/emsdk_env.sh

export EMSDK_SYSROOT=/Users/anutosh491/work/emsdk/upstream/emscripten/cache/sysroot
export NATIVE_SWIFT_BIN=/Users/anutosh491/work/swift-dev/swift-native-build/bin
export NATIVE_LLVM_BIN=/Users/anutosh491/work/swift-dev/llvm-native-full-build/bin
export LLVM_WASM_CMAKE=/Users/anutosh491/work/swift-dev/llvm-wasm-build/lib/cmake/llvm
export STRING_PROC_SRC=/Users/anutosh491/work/swift-dev/swift-experimental-string-processing

emcmake cmake -G Ninja ../swift \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DCMAKE_SYSTEM_NAME=Emscripten \
  -DCMAKE_SYSTEM_PROCESSOR=wasm32 \
  -DUNIX=TRUE \
  -DCMAKE_C_COMPILER_WORKS=TRUE \
  -DCMAKE_CXX_COMPILER_WORKS=TRUE \
  -DCMAKE_Swift_COMPILER_WORKS=TRUE \
  -DLLVM_COMPILER_CHECKED=TRUE \
  -DSWIFT_STDLIB_BUILD_TYPE=Release \
  -DSWIFT_BUILD_RUNTIME_WITH_HOST_COMPILER=TRUE \
  -DBOOTSTRAPPING_MODE=HOSTTOOLS \
  -DSWIFT_NATIVE_SWIFT_TOOLS_PATH=$NATIVE_SWIFT_BIN \
  -DSWIFT_NATIVE_CLANG_TOOLS_PATH=$NATIVE_LLVM_BIN \
  -DSWIFT_NATIVE_LLVM_TOOLS_PATH=$NATIVE_LLVM_BIN \
  -DLLVM_DIR=$LLVM_WASM_CMAKE \
  -DSWIFT_PATH_TO_CMARK_SOURCE=/Users/anutosh491/work/swift-dev/cmark \
  -DSWIFT_PATH_TO_CMARK_BUILD=/Users/anutosh491/work/swift-dev/cmark-wasm-build \
  -DSWIFT_EMSCRIPTEN_SYSROOT_PATH=$EMSDK_SYSROOT \
  -DSWIFT_INCLUDE_TOOLS=FALSE \
  -DSWIFT_INCLUDE_DOCS=FALSE \
  -DSWIFT_INCLUDE_TESTS=FALSE \
  -DSWIFT_BUILD_REMOTE_MIRROR=FALSE \
  -DSWIFT_BUILD_SOURCEKIT=FALSE \
  -DSWIFT_PRIMARY_VARIANT_SDK=EMSCRIPTEN \
  -DSWIFT_PRIMARY_VARIANT_ARCH=wasm32 \
  -DSWIFT_SDKS=EMSCRIPTEN \
  -DSWIFT_BUILD_STATIC_STDLIB=TRUE \
  -DSWIFT_BUILD_DYNAMIC_STDLIB=FALSE \
  -DSWIFT_BUILD_STATIC_SDK_OVERLAY=TRUE \
  -DSWIFT_STDLIB_INSTALL_ONLY_CLANG_RESOURCE_HEADERS=TRUE \
  -DSWIFT_STDLIB_STABLE_ABI=TRUE \
  -DSWIFT_STDLIB_TRACING=FALSE \
  -DSWIFT_STDLIB_HAS_ASLR=FALSE \
  -DSWIFT_STDLIB_INSTALL_PARENT_MODULE_FOR_SHIMS=FALSE \
  -DSWIFT_RUNTIME_CRASH_REPORTER_CLIENT=FALSE \
  -DSWIFT_STDLIB_SINGLE_THREADED_CONCURRENCY=TRUE \
  -DSWIFT_ENABLE_DISPATCH=FALSE \
  -DSWIFT_STDLIB_SUPPORTS_BACKTRACE_REPORTING=FALSE \
  -DSWIFT_STDLIB_HAS_DLADDR=FALSE \
  -DSWIFT_STDLIB_COMPACT_ABSOLUTE_FUNCTION_POINTER=TRUE \
  -DSWIFT_ENABLE_EXPERIMENTAL_CONCURRENCY=TRUE \
  -DSWIFT_ENABLE_EXPERIMENTAL_DISTRIBUTED=TRUE \
  -DSWIFT_ENABLE_EXPERIMENTAL_STRING_PROCESSING=TRUE \
  -DSWIFT_PATH_TO_STRING_PROCESSING_SOURCE=$STRING_PROC_SRC \
  -DSWIFT_ENABLE_EXPERIMENTAL_CXX_INTEROP=TRUE \
  -DSWIFT_ENABLE_SYNCHRONIZATION=TRUE \
  -DSWIFT_ENABLE_VOLATILE=TRUE \
  -DSWIFT_ENABLE_EXPERIMENTAL_OBSERVATION=TRUE \
  -DSWIFT_ENABLE_EXPERIMENTAL_DIFFERENTIABLE_PROGRAMMING=TRUE \
  -DSWIFT_SHOULD_BUILD_EMBEDDED_STDLIB=FALSE \
  -DSWIFT_SHOULD_BUILD_EMBEDDED_STDLIB_CROSS_COMPILING=FALSE \
  -DSWIFT_THREADING_PACKAGE=none \
  "-DSWIFT_STDLIB_EXTRA_C_COMPILE_FLAGS=-isystem;$EMSDK_SYSROOT/include/compat" \
  "-DSWIFT_STDLIB_EXTRA_SWIFT_COMPILE_FLAGS=-no-verify-emitted-module-interface"

# Build all stdlib targets — ~30-60 min
# If already configured (swift-stdlib-wasm-build/ exists), just re-run ninja — no reconfigure needed.
ninja -j$(sysctl -n hw.logicalcpu)
```

**How cmake routes swiftc calls:**
`emcmake` sets `CMAKE_C_COMPILER=emcc` / `CMAKE_CXX_COMPILER=em++` for C/C++ parts.
Swift source files are compiled by the native swiftc at `SWIFT_NATIVE_SWIFT_TOOLS_PATH`
(Phase 5b), which receives `-target wasm32-unknown-emscripten -sysroot $EMSDK_SYSROOT` from
the cmake infrastructure — it knows how to emit wasm32 objects thanks to Max's patches and
the WebAssembly backend from Phase 5a.

Verify after build:
```bash
# .swiftmodule files — NOT in wasm32/ subdir, they live one level up at emscripten/
ls /Users/anutosh491/work/swift-dev/swift-stdlib-wasm-build/lib/swift/emscripten/Swift.swiftmodule
ls /Users/anutosh491/work/swift-dev/swift-stdlib-wasm-build/lib/swift/emscripten/_Concurrency.swiftmodule

# Static runtime libs — in lib/swift_static/ (not lib/swift/)
ls /Users/anutosh491/work/swift-dev/swift-stdlib-wasm-build/lib/swift_static/emscripten/wasm32/libswiftCore.a
ls /Users/anutosh491/work/swift-dev/swift-stdlib-wasm-build/lib/swift_static/emscripten/wasm32/libswift_Concurrency.a
```

**Artifact layout note:** `.swiftmodule` bundles sit at `lib/swift/emscripten/<Module>.swiftmodule/`
(architecture-tagged `.swiftmodule` files inside, e.g. `Swift.swiftmodule/wasm32-unknown-emscripten.swiftmodule`).
Static libs sit at `lib/swift_static/emscripten/wasm32/lib<Module>.a`.

The `SearchPathOpts.RuntimeLibraryPaths` in the `CompilerInstance` (Phase 6) must include
`swift-stdlib-wasm-build/lib/swift` for the type-checker to find `Swift.swiftmodule`.

---

### Phase 6 — `swift-repl-wasm` Project

Analogous to `clang-repl-wasm`. Project layout:

```
swift-dev/swift-repl-wasm/
  SwiftCompilerModule.cpp   ← C++ glue module, exports REPL functions to JS
  CMakeLists.txt            ← emcmake cmake entry point
```

---

#### Question 1: What to Preload in the Emscripten Virtual FS

In `clang-repl-wasm`, two things are preloaded:
1. The emsdk sysroot (`sysroot@/`) — C/C++ system headers for the clang frontend
2. Clang's resource dir (implicit in the sysroot) — compiler-rt builtins

For `swift-repl-wasm`, the equivalent preloads are:

**1. Swift stdlib `.swiftmodule` files** — the type-checker needs these to resolve `import Swift`.
The `.swiftmodule` bundles sit at `lib/swift/emscripten/` (not in a `wasm32/` subdir).
Map the whole `lib/swift` tree into the VFS at `/lib/swift`:

```
--preload-file /Users/anutosh491/work/swift-dev/swift-stdlib-wasm-build/lib/swift@/lib/swift
```

This makes `Swift.swiftmodule`, `_Concurrency.swiftmodule`, etc. accessible at
`/lib/swift/emscripten/Swift.swiftmodule` inside the wasm module. The `CompilerInstance`
must set `SearchPathOpts.RuntimeLibraryPaths` to `/lib/swift` so the type-checker finds them.

**2. emsdk sysroot** — needed for C interop (`import Glibc`, `import Darwin`, etc.) and for
Clang's ClangImporter which the Swift type-checker uses to import C modules:

```
--preload-file /Users/anutosh491/work/emsdk/upstream/emscripten/cache/sysroot@/
```

This is identical to `clang-repl-wasm`. Maps the entire emscripten sysroot (headers, crt,
libc++, etc.) to the VFS root `/`.

**Note on clang resource dir:** Clang's own resource headers (compiler-rt, stddef.h, etc.)
ship inside the emsdk sysroot (`sysroot/lib/clang/<version>/include/`), so preloading the
sysroot implicitly handles this — no separate preload needed.

---

#### Question 2: What the MAIN_MODULE Links Against

The emcc link step must include all transitive static libs:

**A. Swift compiler libs** (from `swift-wasm-build/lib/`):

```
libswiftImmediate.a
libswiftFrontend.a
libswiftIRGen.a
libswiftSILGen.a
libswiftSILOptimizer.a
libswiftSIL.a
libswiftSema.a
libswiftAST.a
libswiftParse.a
libswiftClangImporter.a
libswiftSerialization.a
libswiftIDE.a
libswiftBasic.a
libswiftDemangling.a
libswiftMarkup.a
libswiftOption.a
libswiftSwiftOnoneSupport.a
```

Plus `swiftCompilerStub` — provides the no-op `initializeSwiftModules()` when
`SWIFT_ENABLE_SWIFT_IN_SWIFT=OFF`. This is built as an OBJECT library, not included in
`libswiftImmediate.a`. Link it as an explicit `.o`:
```
swift-wasm-build/SwiftCompilerSources/CMakeFiles/swiftCompilerStub.dir/stubs.cpp.o
```

**B. LLVM+LLD static libs** (from `llvm-wasm-build/lib/`):

```
-lLLVMSupport -lLLVMCore -lLLVMTargetParser
-lLLVMAnalysis -lLLVMTarget -lLLVMTransformUtils -lLLVMIRPrinter -lLLVMIRReader
-lLLVMBitReader -lLLVMBitWriter -lLLVMBitstreamReader
-lLLVMCoroutines -lLLVMCoverage -lLLVMDebugInfoDWARF
-lLLVMInstrumentation -lLLVMipo -lLLVMLTO -lLLVMMC -lLLVMMCParser
-lLLVMObject -lLLVMObjCARCOpts -lLLVMOption -lLLVMProfileData -lLLVMRemarks
-lLLVMWebAssemblyCodeGen -lLLVMWebAssemblyAsmParser
-lLLVMWebAssemblyDesc -lLLVMWebAssemblyInfo
-llldCommon -llldWasm
-lclangBasic -lclangLex -lclangParse -lclangAST -lclangASTMatchers
-lclangAnalysis -lclangEdit -lclangRewrite -lclangSerialization
-lclangFrontend -lclangDriver -lclangCodeGen
```

`lldWasm` + `lldCommon` enable in-process `wasm-ld` linking per REPL cell (same as
`clang-repl-wasm`'s use of `WasmIncrementalExecutor`).

**C. Swift runtime** (from `swift-stdlib-wasm-build/lib/swift_static/emscripten/wasm32/`):

```
libswiftCore.a
libswift_Concurrency.a
libswiftDistributed.a
libswiftObservation.a
libswiftRegexBuilder.a
libswift_StringProcessing.a
libswift_RegexParser.a
libswiftSynchronization.a
libswiftEmscriptenLibc.a
```

These are baked into the MAIN_MODULE so that each cell's SIDE_MODULE can resolve Swift
runtime symbols (`swift_retain`, `swift_release`, `swift_once`, `_swift_stdlib_*`, etc.)
at `dlopen()` time without needing separate shared libs.

**D. Other deps** (from `$WASM_PREFIX/lib/`):

```
libzstd.a   ← Swift uses zstd for serialization (swiftmodule compression)
libuuid.a   ← Swift runtime UUID support
libcmark-gfm.a libcmark-gfmExtensions.a  ← Swift frontend Markdown parser
```

---

#### Question 3: The `autolinkImportedModules` Guard — Remove It

The current `Interpreter.cpp` guards the `autolinkImportedModules()` call with
`#ifndef __EMSCRIPTEN__`. This was added as a conservative safety measure (native `.dylib`
paths don't exist in wasm). **This guard should be removed or rethought.**

**Why the guard can be removed:**

`autolinkImportedModules()` calls `dlopen()` on the library paths associated with each
Swift module imported in the cell. In the wasm browser context:

- Emscripten's `dlopen()` is a real implementation, not a stub. It works correctly in the
  browser — confirmed by `clang-repl-wasm` which uses `dlopen()` to load each compiled
  cell as a SIDE_MODULE.
- We can preload any required `.wasm` side modules into the Emscripten VFS at startup with
  `--preload-file`. The `dlopen()` call will find them there.
- If a user does `import MyModule` and `MyModule`'s associated wasm side module is in the
  VFS (or linked into the MAIN_MODULE), `autolinkImportedModules()` can load it just as
  well as on macOS.

The only real caveat is that the path the Swift frontend associates with a module must be a
VFS path (e.g. `/lib/swift/emscripten/`) rather than a macOS absolute path. This is
controlled by the `SearchPathOpts` and `RuntimeLibraryPaths` set in the `CompilerInvocation`
— they should point to VFS-preloaded paths like `/lib/swift`.

**Summary:** Remove `#ifndef __EMSCRIPTEN__` from the `autolinkImportedModules()` call.
The call is safe in wasm as long as the module search paths resolve into the VFS.

---

#### `SwiftCompilerModule.cpp` Skeleton

```cpp
#include <emscripten/emscripten.h>
#include "swift/Immediate/Interpreter.h"
#include "swift/Frontend/Frontend.h"
#include "swift/Frontend/FrontendOptions.h"
#include "swift/Frontend/PrintingDiagnosticConsumer.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/Basic/SourceManager.h"
#include "llvm/Support/TargetSelect.h"
#include <memory>
#include <mutex>

static std::unique_ptr<swift::CompilerInstance> s_CI;
static std::unique_ptr<swift::Interpreter> s_interp;
static swift::PrintingDiagnosticConsumer s_diagPrinter;

extern "C" {

EMSCRIPTEN_KEEPALIVE int swift_repl_init() {
  static std::once_flag initOnce;
  std::call_once(initOnce, [] {
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmPrinters();
    llvm::InitializeAllAsmParsers();
  });

  swift::CompilerInvocation invocation;
  std::vector<const char *> args = {
    "swift-repl",
    "-target", "wasm32-unknown-emscripten",
    // VFS-preloaded stdlib .swiftmodule files are at /lib/swift/emscripten/
    "-resource-dir", "/lib/swift",
    // emsdk sysroot is preloaded at VFS root
    "-sdk", "/",
    "-enable-experimental-concurrency",
  };

  llvm::SmallVector<std::string, 4> errs;
  swift::DiagnosticEngine diags(*(new swift::SourceManager()));
  if (invocation.parseArgs(args, diags)) return -1;

  invocation.getFrontendOptions().RequestedAction =
      swift::FrontendOptions::ActionType::REPL;

  s_CI = std::make_unique<swift::CompilerInstance>();
  std::string setupError;
  if (s_CI->setup(invocation, setupError)) return -1;
  s_CI->addDiagnosticConsumer(&s_diagPrinter);

  // parseStdlib=false — load the prebuilt .swiftmodule instead of parsing source
  llvm::Error err = llvm::Error::success();
  s_interp = std::make_unique<swift::Interpreter>(
      *s_CI,
      swift::ProcessCmdLine(args.data(), args.data() + args.size()),
      /*parseStdlib=*/false,
      err);
  if (err) return -1;
  return 0;
}

EMSCRIPTEN_KEEPALIVE int swift_repl_parse_and_execute(const char *code) {
  if (!s_interp) return -1;
  llvm::Error err = s_interp->parseAndExecute(code);
  if (err) {
    llvm::consumeError(std::move(err));
    return -1;
  }
  return 0;
}

EMSCRIPTEN_KEEPALIVE int swift_repl_is_complete(const char *code) {
  if (!s_interp) return 1;
  return s_interp->isInputComplete(code) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE const char *swift_repl_complete(const char *code) {
  // TODO: wrap s_interp->complete() results into a null-terminated JSON array
  return "[]";
}

} // extern "C"
```

---

#### `CMakeLists.txt` for `swift-repl-wasm`

```cmake
cmake_minimum_required(VERSION 3.20)
project(swift-repl-wasm)

set(CMAKE_CXX_STANDARD 17)

# Wasm Swift compiler libs
set(SWIFT_WASM_BUILD /Users/anutosh491/work/swift-dev/swift-wasm-build)
# Wasm LLVM+LLD+Clang libs
set(LLVM_WASM_BUILD /Users/anutosh491/work/swift-dev/llvm-wasm-build)
# Wasm Swift stdlib static libs
set(STDLIB_WASM_BUILD /Users/anutosh491/work/swift-dev/swift-stdlib-wasm-build)
set(WASM_PREFIX /Users/anutosh491/micromamba/envs/swift-wasm)
set(EMSDK_SYSROOT /Users/anutosh491/work/emsdk/upstream/emscripten/cache/sysroot)

find_package(LLVM REQUIRED CONFIG
  PATHS ${LLVM_WASM_BUILD}/lib/cmake/llvm NO_DEFAULT_PATH)
find_package(LLD REQUIRED CONFIG
  PATHS ${LLVM_WASM_BUILD}/lib/cmake/lld NO_DEFAULT_PATH)
find_package(Clang REQUIRED CONFIG
  PATHS ${LLVM_WASM_BUILD}/lib/cmake/clang NO_DEFAULT_PATH)

include_directories(
  ${LLVM_INCLUDE_DIRS}
  ${CLANG_INCLUDE_DIRS}
  ${SWIFT_WASM_BUILD}/include
)

add_compile_options(-fwasm-exceptions)

add_executable(SwiftRepl SwiftCompilerModule.cpp)

# Swift compiler libs (order matters: leaf libs first for static linking)
set(SWIFT_LIBS
  ${SWIFT_WASM_BUILD}/lib/libswiftImmediate.a
  ${SWIFT_WASM_BUILD}/lib/libswiftIDE.a
  ${SWIFT_WASM_BUILD}/lib/libswiftFrontend.a
  ${SWIFT_WASM_BUILD}/lib/libswiftIRGen.a
  ${SWIFT_WASM_BUILD}/lib/libswiftSILGen.a
  ${SWIFT_WASM_BUILD}/lib/libswiftSILOptimizer.a
  ${SWIFT_WASM_BUILD}/lib/libswiftSIL.a
  ${SWIFT_WASM_BUILD}/lib/libswiftSema.a
  ${SWIFT_WASM_BUILD}/lib/libswiftAST.a
  ${SWIFT_WASM_BUILD}/lib/libswiftParse.a
  ${SWIFT_WASM_BUILD}/lib/libswiftClangImporter.a
  ${SWIFT_WASM_BUILD}/lib/libswiftSerialization.a
  ${SWIFT_WASM_BUILD}/lib/libswiftBasic.a
  ${SWIFT_WASM_BUILD}/lib/libswiftDemangling.a
  ${SWIFT_WASM_BUILD}/lib/libswiftMarkup.a
  ${SWIFT_WASM_BUILD}/lib/libswiftOption.a
  # no-op stub for initializeSwiftModules (SWIFT_ENABLE_SWIFT_IN_SWIFT=OFF):
  ${SWIFT_WASM_BUILD}/SwiftCompilerSources/CMakeFiles/swiftCompilerStub.dir/stubs.cpp.o
)

# Swift runtime (baked into MAIN_MODULE — cells resolve against these via dlopen)
set(SWIFT_RUNTIME_LIBS
  ${STDLIB_WASM_BUILD}/lib/swift_static/emscripten/wasm32/libswiftCore.a
  ${STDLIB_WASM_BUILD}/lib/swift_static/emscripten/wasm32/libswift_Concurrency.a
  ${STDLIB_WASM_BUILD}/lib/swift_static/emscripten/wasm32/libswiftObservation.a
  ${STDLIB_WASM_BUILD}/lib/swift_static/emscripten/wasm32/libswiftSynchronization.a
  ${STDLIB_WASM_BUILD}/lib/swift_static/emscripten/wasm32/libswift_StringProcessing.a
  ${STDLIB_WASM_BUILD}/lib/swift_static/emscripten/wasm32/libswift_RegexParser.a
  ${STDLIB_WASM_BUILD}/lib/swift_static/emscripten/wasm32/libswiftEmscriptenLibc.a
)

target_link_libraries(SwiftRepl
  ${SWIFT_LIBS}
  ${SWIFT_RUNTIME_LIBS}
  lldWasm lldCommon
  LLVMWebAssemblyCodeGen LLVMWebAssemblyAsmParser LLVMWebAssemblyDesc LLVMWebAssemblyInfo
  LLVMipo LLVMLinker LLVMIRPrinter LLVMIRReader LLVMBitWriter LLVMBitReader
  LLVMBitstreamReader LLVMCoroutines LLVMCoverage LLVMDebugInfoDWARF
  LLVMInstrumentation LLVMLTO LLVMObjCARCOpts LLVMProfileData LLVMRemarks
  LLVMMCParser LLVMMC LLVMObject LLVMTarget LLVMTransformUtils
  LLVMAnalysis LLVMCore LLVMOption LLVMTargetParser LLVMSupport
  clangCodeGen clangFrontend clangDriver clangSerialization clangRewrite
  clangEdit clangAnalysis clangASTMatchers clangAST clangParse clangLex clangBasic
  ${WASM_PREFIX}/lib/libzstd.a
  ${WASM_PREFIX}/lib/libuuid.a
  ${WASM_PREFIX}/lib/libcmark-gfm.a
  ${WASM_PREFIX}/lib/libcmark-gfmExtensions.a
  embind
)

target_link_options(SwiftRepl PUBLIC
  "SHELL:-O1"
  "SHELL:-fwasm-exceptions"
  "SHELL:-sMODULARIZE=1"
  "SHELL:-sEXPORT_ES6=1"
  "SHELL:-sASSERTIONS=0"
  "SHELL:-sALLOW_MEMORY_GROWTH=1"
  "SHELL:-sINITIAL_MEMORY=256MB"
  "SHELL:-sTOTAL_STACK=32MB"
  "SHELL:-sMAIN_MODULE=1"
  "SHELL:-sEXPORTED_RUNTIME_METHODS=ccall,cwrap,stringToNewUTF8,FS"
  "SHELL:-sEXPORTED_FUNCTIONS=_malloc,_free,_swift_repl_init,_swift_repl_parse_and_execute,_swift_repl_is_complete,_swift_repl_complete"
  # Preload Swift .swiftmodule files → type-checker finds them at /lib/swift/emscripten/
  "SHELL:--preload-file=${STDLIB_WASM_BUILD}/lib/swift@/lib/swift"
  # Preload emsdk sysroot → C interop headers, Clang resource headers
  "SHELL:--preload-file=${EMSDK_SYSROOT}@/"
)

set_target_properties(SwiftRepl PROPERTIES OUTPUT_NAME "SwiftRepl")
```

Build command:
```bash
cd /Users/anutosh491/work/swift-dev/swift-repl-wasm
mkdir -p build && cd build
emcmake cmake -DCMAKE_BUILD_TYPE=Release ..
emmake make -j$(sysctl -n hw.logicalcpu)
```

Output: `SwiftRepl.js`, `SwiftRepl.wasm`, `SwiftRepl.data`

---

### Phase 7 — The Full Runtime Picture

```
Browser                          SwiftRepl.wasm (MAIN_MODULE)
  │                                    │
  │  JS: Module._swift_repl_init()     │
  │───────────────────────────────────>│
  │                                    │  reads Swift.swiftmodule from VFS
  │                                    │  at /lib/swift/emscripten/
  │                                    │  type-checks stdlib warmup
  │                                    │
  │  JS: Module._swift_repl_parse_and_execute("print(\"hello\")")
  │───────────────────────────────────>│
  │                                    │  typeCheck → SIL → IRGen → __repl_0.o
  │                                    │  wasm-ld (lldWasm in-process):
  │                                    │    __repl_0.o → __repl_0.wasm (SIDE_MODULE)
  │                                    │    --allow-undefined (Swift runtime)
  │                                    │  dlopen(__repl_0.wasm)
  │                                    │    resolves swift_retain, swift_release,
  │                                    │    _swift_stdlib_*, print() etc. from MAIN_MODULE
  │                                    │  dlsym("$s__repl_0AA...") → call()
  │                                    │  stdout: "hello"
  │<───────────────────────────────────│
```

**Key insight:** The MAIN_MODULE bakes in `libswiftCore.a` and all Swift runtime libs.
Each cell's SIDE_MODULE imports those symbols at `dlopen()` time using Emscripten's wasm
dynamic linking — no JIT required.
