#!/usr/bin/env bash
# Builds LLVM and lld to run as WebAssembly, for wasm32-wasip1, and gathers
# them into one archive a WebAssembly program links against.
#
#     build-for-wasi.sh <llvm-project> <wasi-sdk> <work directory> <this repository> <version>
#
# What comes out is <work directory>/llvm-wasi: lib/libLLVM-wasi.a and include/.
#
# **Why one archive.** A program built for WASI cannot load anything at run
# time, so everything it calls is linked into it - LLVM, lld, the C interface
# to lld, and the C and C++ libraries those are written against. Handing all
# of that over as one file is what lets a consumer name one thing to link
# with, and a linker only takes the members something actually asked for, so
# it costs nothing in what is linked.
#
# **Two builds, because LLVM writes part of its own source.** TableGen has to
# run on the machine doing the building, so a native one is built first and
# the WebAssembly build is pointed at it.
#
# **Only the libraries, and not all of them.** The ones that cannot work in
# WebAssembly - running code as it is compiled above all - are not built, and
# a program reaching for one finds it missing at its link rather than here.
set -euo pipefail

source=$(realpath "$1")
sdk=$(realpath "$2")
mkdir -p "$3"
work=$(realpath "$3")
repository=$(realpath "$4")
version=$5

jobs=${JOBS:-$(nproc)}

# LLVM does not build for WASI as it is released; the patch beside this says
# where it comes from. Applied unless it already has been, so a second run in
# the same tree goes on rather than failing here.
patch="$repository/patches/wasi-llvm-$version.patch"

if git -C "$source" apply --check --reverse "$patch" 2>/dev/null; then
  echo "the WASI patch is applied already"
else
  git -C "$source" apply "$patch"
fi

# TableGen, for this machine. Nothing else of the native build is wanted.
cmake -S "$source/llvm" -B "$work/native" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_TARGETS_TO_BUILD=WebAssembly \
  -DLLVM_ENABLE_PROJECTS=lld \
  -DLLVM_INCLUDE_TESTS=OFF \
  -DLLVM_INCLUDE_EXAMPLES=OFF \
  -DLLVM_INCLUDE_BENCHMARKS=OFF \
  -DLLVM_ENABLE_ZLIB=OFF \
  -DLLVM_ENABLE_ZSTD=OFF \
  -DLLVM_ENABLE_LIBXML2=OFF

ninja -C "$work/native" -j "$jobs" llvm-tblgen llvm-min-tblgen

# What WASI leaves out and LLVM still mentions: mapping memory, signals, the
# time a process has used and its number. wasi-libc emulates each of them
# when asked, and these are how it is asked.
emulated="-D_WASI_EMULATED_MMAN -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS -D_WASI_EMULATED_GETPID"

cmake -S "$source/llvm" -B "$work/wasi" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$sdk/share/cmake/wasi-sdk-p1.cmake" \
  -DWASI_SDK_PREFIX="$sdk" \
  -DCMAKE_BUILD_TYPE=MinSizeRel \
  -DCMAKE_C_FLAGS="$emulated" \
  -DCMAKE_CXX_FLAGS="$emulated" \
  -DLLVM_HOST_TRIPLE=wasm32-wasip1 \
  -DLLVM_DEFAULT_TARGET_TRIPLE=wasm32-wasip1 \
  -DLLVM_TARGETS_TO_BUILD=WebAssembly \
  -DLLVM_ENABLE_PROJECTS=lld \
  -DLLVM_NATIVE_TOOL_DIR="$work/native/bin" \
  -DLLVM_ENABLE_THREADS=OFF \
  -DLLVM_ENABLE_PIC=OFF \
  -DLLVM_ENABLE_PLUGINS=OFF \
  -DLLVM_ENABLE_BACKTRACES=OFF \
  -DLLVM_ENABLE_CRASH_OVERRIDES=OFF \
  -DLLVM_ENABLE_UNWIND_TABLES=OFF \
  -DLLVM_ENABLE_ASSERTIONS=OFF \
  -DLLVM_ENABLE_ZLIB=OFF \
  -DLLVM_ENABLE_ZSTD=OFF \
  -DLLVM_ENABLE_LIBXML2=OFF \
  -DLLVM_ENABLE_TERMINFO=OFF \
  -DLLVM_ENABLE_LIBEDIT=OFF \
  -DLLVM_ENABLE_LIBPFM=OFF \
  -DLLVM_INCLUDE_TESTS=OFF \
  -DLLVM_INCLUDE_EXAMPLES=OFF \
  -DLLVM_INCLUDE_BENCHMARKS=OFF \
  -DLLVM_INCLUDE_DOCS=OFF \
  -DLLVM_INCLUDE_UTILS=OFF \
  -DLLVM_BUILD_TOOLS=OFF \
  -DLLVM_BUILD_UTILS=OFF

# The libraries the C interface reaches and the WebAssembly linker needs; each
# brings what it depends on. lldWasm alone pulls in most of LLVM, by way of
# the link-time optimiser.
ninja -C "$work/wasi" -j "$jobs" \
  LLVMCore LLVMAnalysis LLVMBitReader LLVMBitWriter LLVMIRReader LLVMAsmParser \
  LLVMPasses LLVMTarget LLVMTransformUtils LLVMLinker LLVMObject LLVMMC LLVMMCParser \
  LLVMWebAssemblyCodeGen LLVMWebAssemblyAsmParser LLVMWebAssemblyDesc LLVMWebAssemblyInfo \
  LLVMWebAssemblyDisassembler LLVMWebAssemblyUtils \
  lldWasm lldCommon

# The C interface to lld, compiled straight against the build tree: it is one
# file, and an install to find it through is what the native build needs and
# this one does not.
clang="$sdk/bin/clang++ --sysroot=$sdk/share/wasi-sysroot --target=wasm32-wasip1"

$clang -c -Os -std=c++17 -fno-rtti -fno-exceptions $emulated \
  -I "$source/llvm/include" -I "$work/wasi/include" \
  -I "$source/lld/include" -I "$work/wasi/tools/lld/include" \
  -I "$repository/shim/include" \
  -DLLD_C_BUILDING=1 -DLLD_C_VERSION="\"$version\"" \
  -DLLD_C_HAS_COFF=0 -DLLD_C_HAS_ELF=0 -DLLD_C_HAS_MINGW=0 -DLLD_C_HAS_MACHO=0 -DLLD_C_HAS_WASM=1 \
  "$repository/shim/lld_c.cpp" -o "$work/lld_c.o"

# What LLVM mentions and WASI has not got; the file says what each answers.
"$sdk/bin/clang" --sysroot="$sdk/share/wasi-sysroot" --target=wasm32-wasip1 -c -Os \
  "$repository/shim/wasi_stubs.c" -o "$work/wasi_stubs.o"

# Everything into one archive: what was built, the shim, and the C and C++
# libraries from the SDK with the emulations above.
sysroot="$sdk/share/wasi-sysroot/lib/wasm32-wasip1"
builtins=$(find "$sdk/lib/clang" -path '*wasm32*' -name 'libclang_rt.builtins*.a' | head -n 1)

out="$work/llvm-wasi"
rm -rf "$out"
mkdir -p "$out/lib" "$out/include"

{
  echo "create $out/lib/libLLVM-wasi.a"
  for library in "$work"/wasi/lib/*.a; do
    echo "addlib $library"
  done
  for library in libc.a libwasi-emulated-mman.a libwasi-emulated-signal.a \
                 libwasi-emulated-process-clocks.a libwasi-emulated-getpid.a; do
    echo "addlib $sysroot/$library"
  done
  # The SDK builds its C++ library with exceptions and without; LLVM is built
  # without them, so those are the ones.
  for library in libc++.a libc++abi.a; do
    echo "addlib $sysroot/noeh/$library"
  done
  echo "addlib $builtins"
  echo "addmod $work/lld_c.o"
  echo "addmod $work/wasi_stubs.o"
  echo "save"
  echo "end"
} > "$work/gather.mri"

"$sdk/bin/llvm-ar" -M < "$work/gather.mri"
"$sdk/bin/llvm-ranlib" "$out/lib/libLLVM-wasi.a"

# The headers, for whoever wants to see what the archive offers.
cp -r "$source/llvm/include/llvm-c" "$out/include/"
cp -r "$repository/shim/include/lld-c" "$out/include/"

ls -la "$out/lib"
