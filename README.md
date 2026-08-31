# build-llvm

LLVM built and released, so a project downloads one rather than having one
installed.

Actions → **Build LLVM** → Run workflow. Nothing to fill in. Takes hours.

Each asset unpacks to an install prefix — `include/`, `lib/`, `lib/cmake`, and
the C API shared library in `bin/`. Point `CMAKE_PREFIX_PATH` at it.

One more asset holds C# declarations for the C API, read out of the headers the
same run built, so they cannot disagree with the library.

## lld gets a C interface, because it ships none

lld's entry points are C++ — mangled names taking `llvm::ArrayRef` and
`llvm::raw_ostream` references, in static libraries rather than the shared one.
So nothing outside C++ can reach the linker at all, however well it can reach
the rest of LLVM through `llvm-c`. `shim/` is the whole of what fills that gap:
a small `lld-c` library beside `LLVM-C`, with `include/lld-c/Driver.h` beside
`include/llvm-c`, and C# declarations for it in the bindings asset.

It covers what lld actually offers rather than what one caller happened to need:
both entry points (the re-entrant one that recovers from a crash, and the direct
driver call with its `exitEarly` and `disableOutput` controls), which drivers
this build has, the version, and letting go of what a link left standing.

Building it here rather than in whatever project wants it means one build per
platform instead of one per developer, no C++ toolchain needed downstream, and a
shim that cannot be a version behind the LLVM it wraps.

Six machines: Windows, Linux and macOS, each on x64 and arm64.

The latest LLVM release, X86 and AArch64, with lld, without zlib/zstd/libxml2,
without the DIA SDK, and without tools. The workflow says why for each.

## Two things the run checks before releasing anything

Both were found by a project trying to use the 23.1.0 assets, hours after the
run that made them looked like it had succeeded.

**That the result can be built against.** A `find_package(LLVM CONFIG)` against
the freshly installed prefix, which is the first thing a consumer does. Built
with the DIA SDK on, LLVM exports `LLVMDebugInfoPDB` carrying a link dependency
on `DIASDK::Diaguids`, and any machine without a full Visual Studio then fails
to configure at all.

**That the bindings compile.** They do not, as the generator leaves them: every
declaration is emitted once per header that pulled it in, and the
target-initialising functions are called by the wrappers and declared by no
header at all — LLVM writes those with a macro over the back ends the build was
configured with. `.github/scripts/prepare-bindings.py` deals with both, and
compiling them here is what keeps that a checked claim.
