/*===-- lld-c/Driver.h - C interface to lld ------------------------*- C -*-===*\
|*                                                                            *|
|* A C interface to lld, which ships none of its own.                         *|
|*                                                                            *|
|* lld's entry points are C++: mangled names taking llvm::ArrayRef and        *|
|* llvm::raw_ostream references, in static libraries rather than the shared   *|
|* one. So nothing outside C++ can reach the linker at all, however well it   *|
|* can reach the rest of LLVM through llvm-c. This is the whole of what fills  *|
|* that gap, and it is built beside LLVM by the run that built LLVM, so the   *|
|* two cannot come to disagree.                                              *|
|*                                                                            *|
|* Nothing of LLVM's or of C++'s crosses this boundary. Strings go in and     *|
|* strings come back, so a caller in any language that can call C can link,   *|
|* and a caller that has its own copy of LLVM need not have the same one.     *|
|*                                                                            *|
|* Every string handed back is the caller's to give back with                 *|
|* LLDDisposeMessage. Both sides of that allocation are inside this library,  *|
|* which is the only way it is safe across the boundary.                      *|
|*                                                                            *|
\*===----------------------------------------------------------------------===*/

#ifndef LLD_C_DRIVER_H
#define LLD_C_DRIVER_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * What marks a function as one this library offers.
 *
 * Windows needs it said, and says it differently either side of the boundary:
 * a compiler building the library has to be told to put the name in the export
 * table, and nothing else does. Elsewhere it is the other way round - every
 * name is offered unless something says otherwise - so what is needed there is
 * a way of saying "this one, whatever the default".
 *
 * A consumer gets a plain declaration, which is right for reaching the library
 * by name at run time and right for linking against the import library.
 */
#if defined(_WIN32)
  #if defined(LLD_C_BUILDING)
    #define LLD_C_API __declspec(dllexport)
  #else
    #define LLD_C_API
  #endif
#else
  #define LLD_C_API __attribute__((visibility("default")))
#endif

/**
 * One driver per kind of object file.
 *
 * Which of these a build has depends on how lld was configured, so ask with
 * LLDHasFlavor rather than assuming: linking through one that is not there
 * fails rather than crashing, but it fails for a reason worth knowing before
 * the link rather than after.
 *
 * The numbers are lld's own, so that adding a flavour there does not renumber
 * one here.
 */
typedef enum {
  LLDFlavorInvalid = 0,
  LLDFlavorGnu = 1,     /**< ELF, as ld.lld */
  LLDFlavorMinGW = 2,   /**< ELF conventions with a COFF output, as ld.lld */
  LLDFlavorWinLink = 3, /**< COFF, as lld-link */
  LLDFlavorDarwin = 4,  /**< Mach-O, as ld64.lld */
  LLDFlavorWasm = 5     /**< WebAssembly, as wasm-ld */
} LLDFlavor;

/**
 * How a link went.
 *
 * ReturnCode is zero for success, as it is for the linker run as a program.
 *
 * CanRunAgain is zero where lld recovered from a crash of its own, which may
 * have left memory in a state a second link would fall over on. It is reported
 * rather than acted on: what to do about it belongs to whoever is linking, and
 * a program that has more to do than this one link may want to finish it.
 */
typedef struct {
  int ReturnCode;
  int CanRunAgain;
} LLDResult;

/**
 * How many drivers this build has.
 */
LLD_C_API unsigned LLDCountFlavors(void);

/**
 * The driver at that position, counting from zero, or LLDFlavorInvalid past
 * the end.
 */
LLD_C_API LLDFlavor LLDFlavorAtIndex(unsigned Index);

/**
 * Whether this build has that driver. Zero or one.
 */
LLD_C_API int LLDHasFlavor(LLDFlavor Flavor);

/**
 * What that driver is called - "gnu", "link", "darwin" and so on, which are
 * the names '-flavor' takes. Never null: an unknown one is "invalid".
 *
 * The string belongs to the library and is not to be given back.
 */
LLD_C_API const char *LLDNameOfFlavor(LLDFlavor Flavor);

/**
 * Which lld this is, as a version string.
 *
 * The string belongs to the library and is not to be given back.
 */
LLD_C_API const char *LLDVersion(void);

/**
 * Runs the linker over a command line, choosing the driver the way the lld
 * programs do: from the name in Arguments[0], or from a '-flavor' written
 * after it.
 *
 * This is the entry lld calls safe for re-entry - it recovers from a crash of
 * its own and says so through CanRunAgain, where linking through a driver
 * directly does not.
 *
 * StandardOutput and StandardError are each left holding what the linker wrote
 * to that stream, or null where it wrote nothing. **Pass null for
 * StandardError to have both gathered into StandardOutput**, interleaved in
 * the order the linker wrote them - which is the order that explains a
 * failure, and is lost by keeping them apart. Either may be null where what it
 * holds is not wanted.
 *
 * One link runs at a time inside this library, however many callers there are.
 * lld keeps state for the length of a link that is not per call, and two at
 * once write over each other's and take the process down; a caller that never
 * thought to ask is given the guarantee anyway.
 */
LLD_C_API LLDResult LLDLinkAsProgram(int ArgumentCount, const char *const *Arguments,
                           char **StandardOutput, char **StandardError);

/**
 * Runs the linker over a command line through the driver named, whatever
 * Arguments[0] says.
 *
 * ExitEarly lets the driver end the process on the first error rather than
 * carrying on to report the rest - which is what the linker does when it is a
 * program, and almost never what a library caller wants. DisableOutput stops
 * it writing the file it linked, which is how a caller asks whether a link
 * would work.
 *
 * There is no crash recovery on this route, so CanRunAgain always comes back
 * one. LLDLinkAsProgram is the one that recovers.
 *
 * The streams and the one-at-a-time guarantee are as LLDLinkAsProgram's.
 */
LLD_C_API LLDResult LLDLinkWithFlavor(LLDFlavor Flavor, int ArgumentCount,
                            const char *const *Arguments, int ExitEarly,
                            int DisableOutput, char **StandardOutput,
                            char **StandardError);

/**
 * Gives back a string this library handed out. A null is allowed and does
 * nothing, so a caller need not test before calling.
 */
LLD_C_API void LLDDisposeMessage(char *Message);

/**
 * Whether a link has left anything standing.
 *
 * The two routes differ here, which is worth knowing before asking:
 * LLDLinkAsProgram tidies up after itself and leaves nothing, and
 * LLDLinkWithFlavor leaves its context standing.
 */
LLD_C_API int LLDHasContext(void);

/**
 * Lets go of what a link left standing.
 *
 * Worth calling where CanRunAgain came back zero and the program means to go
 * on doing other things: what a recovered crash left behind is let go of here
 * rather than at exit, where it would be found again as an intermittent crash
 * on the way out.
 */
LLD_C_API void LLDDestroyContext(void);

#ifdef __cplusplus
}
#endif

#endif
