# Builds the C interface to lld against an LLVM that has already been
# installed, and installs it into the same prefix.
#
#     cmake -S shim -B shim-build -DCMAKE_PREFIX_PATH=<install> \
#           -DCMAKE_INSTALL_PREFIX=<install> -DCMAKE_BUILD_TYPE=Release
#     cmake --build shim-build --config Release
#     cmake --install shim-build
#
# A second configure rather than part of LLVM's own, because it has to find
# what LLVM installed rather than what LLVM is building - and finding it the
# same way every other consumer does is what proves the install prefix works.

cmake_minimum_required(VERSION 3.20)

project(arizmo-shims LANGUAGES CXX)

find_package(LLVM REQUIRED CONFIG)
find_package(LLD REQUIRED CONFIG)

message(STATUS "lld-c: LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")

function(compiled_the_way_llvm_was target)
  if(LLVM_ENABLE_RTTI)
    return()
  endif()

  if(MSVC)
    target_compile_options(${target} PRIVATE /GR-)
  else()
    target_compile_options(${target} PRIVATE -fno-rtti)
  endif()
endfunction()

add_library(lld-c SHARED lld_c.cpp)

target_include_directories(lld-c
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${LLVM_INCLUDE_DIRS} ${LLD_INCLUDE_DIRS})

target_compile_definitions(lld-c PRIVATE ${LLVM_DEFINITIONS})

compiled_the_way_llvm_was(lld-c)

# lld's own version is behind lld/Common/Version.inc, which LLVM generates into
# its build tree and does not install - so an installed prefix cannot reach it.
# LLVM's version is the same number, lld being released with it.
target_compile_definitions(lld-c PRIVATE LLD_C_VERSION="${LLVM_PACKAGE_VERSION}")

# Says which side of the boundary this compilation is on, which is what
# LLD_C_API in the header reads to decide whether a name is being offered or
# reached for.
target_compile_definitions(lld-c PRIVATE LLD_C_BUILDING=1)

set_target_properties(lld-c PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  # Everything not exported on purpose stays in: LLVM's own symbols are in here
  # by the static libraries, and a caller that also has LLVM must not find two.
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  WINDOWS_EXPORT_ALL_SYMBOLS OFF)

# Which drivers this build has is settled by which libraries LLVM installed
# rather than by a list written here: a build configured without a back end
# has no library for its driver, and naming one that is not there fails the
# link. The shim asks each of these to decide what to declare.
set(DRIVERS "")

foreach(one COFF:lldCOFF ELF:lldELF MINGW:lldMinGW MACHO:lldMachO WASM:lldWasm)
  string(REPLACE ":" ";" pair "${one}")
  list(GET pair 0 flavor)
  list(GET pair 1 library)

  if(TARGET ${library})
    target_compile_definitions(lld-c PRIVATE LLD_C_HAS_${flavor}=1)
    target_link_libraries(lld-c PRIVATE ${library})
    list(APPEND DRIVERS ${flavor})
  else()
    target_compile_definitions(lld-c PRIVATE LLD_C_HAS_${flavor}=0)
  endif()
endforeach()

message(STATUS "lld-c: drivers ${DRIVERS}")

if(NOT DRIVERS)
  message(FATAL_ERROR "lld-c: this LLVM has no lld drivers, so there is nothing to wrap")
endif()

# lldCommon last, since the drivers reach into it. Only the components lld
# itself uses, rather than all of LLVM: the library is large enough already.
target_link_libraries(lld-c PRIVATE lldCommon)

llvm_map_components_to_libnames(COMPONENTS
  AllTargetsAsmParsers AllTargetsCodeGens AllTargetsDescs AllTargetsInfos
  BinaryFormat Core DebugInfoDWARF LTO MC Object Option Passes Support TargetParser)

target_link_libraries(lld-c PRIVATE ${COMPONENTS})

include(GNUInstallDirs)

install(TARGETS lld-c
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

# A C interface to reading a PDB, which LLVM offers none of either.
#
# The same gap as lld's and filled the same way: llvm::pdb and the CodeView
# readers are templates, ArrayRef and Expected all through, in static libraries
# rather than the shared one, so nothing outside C++ can read a PDB however
# well it can reach the rest of LLVM. A debugger has to.
#
# Built here rather than wherever a debugger is written, for the reasons above:
# one build per platform, no C++ toolchain downstream, and a reader that cannot
# be a version behind the LLVM whose format it reads.
#
# The native reader rather than the DIA one, so it answers on a machine with no
# Visual Studio on it - which is also what lets the LLVM under it be built
# without the DIA SDK, as this one is.
add_library(pdb-c SHARED pdb_c.cpp)

target_include_directories(pdb-c
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${LLVM_INCLUDE_DIRS})

target_compile_definitions(pdb-c PRIVATE ${LLVM_DEFINITIONS})

compiled_the_way_llvm_was(pdb-c)

target_compile_definitions(pdb-c PRIVATE PDB_C_VERSION="${LLVM_PACKAGE_VERSION}")
target_compile_definitions(pdb-c PRIVATE PDB_C_BUILDING=1)

set_target_properties(pdb-c PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  WINDOWS_EXPORT_ALL_SYMBOLS OFF)

llvm_map_components_to_libnames(PDB_COMPONENTS
  BinaryFormat DebugInfoCodeView DebugInfoMSF DebugInfoPDB Object Support
  TargetParser)

target_link_libraries(pdb-c PRIVATE ${PDB_COMPONENTS})

install(TARGETS pdb-c
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
