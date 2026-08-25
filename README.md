# build-llvm

LLVM built and released, so a project downloads one rather than having one
installed.

Actions → **Build LLVM** → Run workflow. Leave `version` empty for LLVM's
latest release. Takes hours.

Each asset unpacks to an install prefix — `include/`, `lib/`, `lib/cmake`, and
the C API shared library in `bin/`. Point `CMAKE_PREFIX_PATH` at it.

With `bindings` on, one more asset: C# declarations for the C API, read out of
the headers the same run built, so they cannot disagree with the library.

Six machines: Windows, Linux and macOS, each on x64 and arm64.

Defaults build X86 and AArch64 with lld, without zlib/zstd/libxml2, and without
tools. The workflow says why.

Nothing here has been run yet.
