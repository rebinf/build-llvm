/*===-- pdb-c/PdbReader.h - C interface to LLVM's native PDB reader --------===*\
|*                                                                            *|
|* Reading a PDB, which LLVM offers no C interface to.                        *|
|*                                                                            *|
|* llvm::pdb and the CodeView readers are C++ only: templates, ArrayRef and    *|
|* Expected all through the surface, in static libraries rather than the       *|
|* shared one. So nothing outside C++ can read a PDB at all, however well it   *|
|* can reach the rest of LLVM through llvm-c. This fills that gap the way      *|
|* lld-c fills lld's.                                                         *|
|*                                                                            *|
|* Everything is in terms of RVAs - offsets from where the image was loaded -  *|
|* so a caller adds the base the operating system actually gave the module.    *|
|* A PDB knows nothing about where a program ends up.                         *|
|*                                                                            *|
|* What it answers is what a debugger asks:                                    *|
|*   a line in a file  -> the addresses to stop at                            *|
|*   an address        -> which line, and which function                      *|
|*   an address        -> what is being held, and where each one sits         *|
|*   a type            -> what it is, and what it holds                       *|
|*                                                                            *|
|* Text handed back belongs to the reader and lives as long as it does. A      *|
|* caller copying it out has nothing to free; a caller keeping the pointer     *|
|* must not outlive the reader it came from.                                   *|
|*                                                                            *|
\*===----------------------------------------------------------------------===*/

#ifndef PDB_C_PDBREADER_H
#define PDB_C_PDBREADER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#if defined(PDB_C_BUILDING)
#define PDB_C_API __declspec(dllexport)
#else
#define PDB_C_API __declspec(dllimport)
#endif
#else
#if defined(PDB_C_BUILDING)
#define PDB_C_API __attribute__((visibility("default")))
#else
#define PDB_C_API
#endif
#endif

/** A PDB that has been read. Everything else takes one of these. */
typedef struct PDBCReaderOpaque *PDBCReaderRef;

/** What a value is, as far as showing it goes. */
typedef enum {
  PDBCTypeKindUnknown = 0,
  /** A number, a truth value or a character: shown by reading its bytes. */
  PDBCTypeKindBasic = 1,
  /** An address, with what is at the end of it named separately. */
  PDBCTypeKindPointer = 2,
  /** Something holding named parts, each at its own offset. */
  PDBCTypeKindComposite = 3,
  /** A set of names, shown by the name rather than the number. */
  PDBCTypeKindEnum = 4,
  /**
   * A run of one type held where it stands. What it holds is named the way a
   * pointer names what is at the end of it, and the size is the whole run, so
   * how many there are is the one divided by the other.
   */
  PDBCTypeKindArray = 5
} PDBCTypeKind;

/**
 * Reads the PDB at the path and hands back what read it, or null.
 *
 * Where it comes back null, *ErrorMessage is set to why, and the caller
 * disposes that with PDBCDisposeMessage. Nothing else is allocated.
 */
PDB_C_API PDBCReaderRef PDBCReaderOpen(const char *Path, char **ErrorMessage);

PDB_C_API void PDBCReaderClose(PDBCReaderRef Reader);

/** Lets go of a message this handed out. */
PDB_C_API void PDBCDisposeMessage(char *Message);

/**
 * The addresses a line in a file turns into, of which there may be several -
 * a line inside something written once and used in several places has one per
 * use.
 *
 * The file is matched on its last part only, and without regard to case: what
 * a PDB holds is the path as the compiler saw it, and an editor asking about
 * an open file has the path as the machine has it now. Those differ over a
 * build that moved, and matching the whole of them would answer nothing.
 *
 * Fills up to HowManyWanted and hands back how many there are, which may be
 * more - ask again with a bigger array to get the rest.
 */
PDB_C_API uint32_t PDBCAddressesOfLine(PDBCReaderRef Reader, const char *File,
                                       uint32_t Line, uint32_t *Addresses,
                                       uint32_t HowManyWanted);

/**
 * Which line an address came from, and which file, or 0 where the address is
 * in nothing this knows about.
 *
 * The address is taken as being inside the line that begins at or before it,
 * which is what an address in the middle of a statement is.
 */
PDB_C_API uint32_t PDBCLineAtAddress(PDBCReaderRef Reader, uint32_t Address,
                                     const char **File);

/** The name of the function an address is inside, or null. */
PDB_C_API const char *PDBCFunctionAtAddress(PDBCReaderRef Reader,
                                            uint32_t Address);

/** Where the function an address is inside begins, or 0. */
PDB_C_API uint32_t PDBCFunctionStartAtAddress(PDBCReaderRef Reader,
                                              uint32_t Address);

/** How many values are held in the function an address is inside. */
PDB_C_API uint32_t PDBCCountLocalsAtAddress(PDBCReaderRef Reader,
                                            uint32_t Address);

/**
 * One of them: its name, how far from the frame register it sits, what type
 * it is, whether it is one of the function's parameters, and whether the
 * register it is measured from is the stack pointer rather than the frame
 * pointer.
 *
 * Which register that is comes from the function itself, so a caller reads
 * whichever one this says rather than assuming.
 */
PDB_C_API const char *PDBCLocalAtAddress(PDBCReaderRef Reader, uint32_t Address,
                                         uint32_t Which, int32_t *FrameOffset,
                                         uint32_t *TypeIndex, int *IsParameter,
                                         int *FromStackPointer);

/**
 * What a type is: its kind, its name, how wide it is, and for an address what
 * is at the end of it.
 *
 * Hands back zero where the type is not one this reads, in which case nothing
 * else is filled in.
 */
PDB_C_API int PDBCTypeInformation(PDBCReaderRef Reader, uint32_t TypeIndex,
                                  PDBCTypeKind *Kind, const char **Name,
                                  uint32_t *SizeInBytes,
                                  uint32_t *PointeeTypeIndex);

/** How many named parts something holds. */
PDB_C_API uint32_t PDBCCountFields(PDBCReaderRef Reader, uint32_t TypeIndex);

/** One of them: its name, how far into the type it sits, and its own type. */
PDB_C_API const char *PDBCFieldAt(PDBCReaderRef Reader, uint32_t TypeIndex,
                                  uint32_t Which, uint32_t *Offset,
                                  uint32_t *FieldTypeIndex);

/** What this shim was built against, which is the LLVM it reads with. */
PDB_C_API const char *PDBCVersion(void);

#ifdef __cplusplus
}
#endif

#endif
