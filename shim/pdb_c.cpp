//===-- pdb_c.cpp - C interface to LLVM's native PDB reader ---------------===//
//
// Everything is read once, when the file is opened, into flat tables sorted by
// address. A debugger asks these questions constantly - every stop, every step,
// every frame - and walking the streams for each would be walking them
// thousands of times over.
//
// Nothing here reaches for the DIA SDK. LLVM's native reader is what this uses,
// which is what lets the same code answer on a machine with no Visual Studio
// on it and what lets the LLVM behind it be built without DIA at all.
//
//===----------------------------------------------------------------------===//

#include "pdb-c/PdbReader.h"

#include "llvm/DebugInfo/CodeView/CVTypeVisitor.h"
#include "llvm/DebugInfo/CodeView/DebugChecksumsSubsection.h"
#include "llvm/DebugInfo/CodeView/DebugLinesSubsection.h"
#include "llvm/DebugInfo/CodeView/DebugSubsectionRecord.h"
#include "llvm/DebugInfo/CodeView/SymbolDeserializer.h"
#include "llvm/DebugInfo/CodeView/SymbolRecord.h"
#include "llvm/DebugInfo/CodeView/TypeDeserializer.h"
#include "llvm/DebugInfo/CodeView/TypeRecord.h"
#include "llvm/DebugInfo/CodeView/TypeVisitorCallbacks.h"
#include "llvm/DebugInfo/PDB/Native/DbiModuleDescriptor.h"
#include "llvm/DebugInfo/PDB/Native/DbiModuleList.h"
#include "llvm/DebugInfo/PDB/Native/DbiStream.h"
#include "llvm/DebugInfo/PDB/Native/ModuleDebugStream.h"
#include "llvm/DebugInfo/PDB/Native/PDBFile.h"
#include "llvm/DebugInfo/PDB/Native/PDBStringTable.h"
#include "llvm/DebugInfo/PDB/Native/TpiStream.h"
#include "llvm/DebugInfo/MSF/MappedBlockStream.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/BinaryByteStream.h"
#include "llvm/Support/BinaryStreamReader.h"
#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace cv = llvm::codeview;
namespace pdb = llvm::pdb;

namespace {

/// The last part of a path, lowercased.
///
/// What a PDB holds is the path as the compiler saw it; what an editor asks
/// about is the path as the machine has it now. Those differ over a build that
/// moved or a drive that is mapped differently, so matching the whole of them
/// would answer nothing. The last part is what both agree on.
///
/// **Used for matching only, never for keeping.** What is kept is the path as
/// the PDB has it, because that is what somebody is handed back when they ask
/// where the program stopped - and a name with no directory in front of it is
/// a place no editor can open.
std::string LastPartLowered(llvm::StringRef Path) {
  size_t Slash = Path.find_last_of("/\\");
  std::string Name =
      (Slash == llvm::StringRef::npos ? Path : Path.substr(Slash + 1)).str();

  std::transform(Name.begin(), Name.end(), Name.begin(),
                 [](unsigned char One) { return std::tolower(One); });

  return Name;
}

/// Gathers the named parts of a CodeView field list.
class MemberCollector : public cv::TypeVisitorCallbacks {
public:
  struct Field {
    std::string Name;
    uint32_t Offset;
    uint32_t TypeIndex;
  };

  std::vector<Field> Fields;

  llvm::Error visitKnownMember(cv::CVMemberRecord &,
                               cv::DataMemberRecord &Record) override {
    Fields.push_back({Record.Name.str(),
                      static_cast<uint32_t>(Record.getFieldOffset()),
                      Record.getType().getIndex()});

    return llvm::Error::success();
  }
};

/// How wide one of the machine's own types is, in bytes.
///
/// Written out rather than asked for, there being nothing to ask: the width of
/// a simple type is in what it is, and CodeView says what it is by number. A
/// value read at the wrong width reads its neighbour as its own high half,
/// which shows as an enormous number and is the only sign anything is wrong.
uint32_t HowWideOneOfTheMachinesOwnIs(cv::SimpleTypeKind Kind) {
  switch (Kind) {
  case cv::SimpleTypeKind::Boolean8:
  case cv::SimpleTypeKind::Byte:
  case cv::SimpleTypeKind::SByte:
  case cv::SimpleTypeKind::SignedCharacter:
  case cv::SimpleTypeKind::UnsignedCharacter:
  case cv::SimpleTypeKind::NarrowCharacter:
    return 1;

  case cv::SimpleTypeKind::Boolean16:
  case cv::SimpleTypeKind::Int16:
  case cv::SimpleTypeKind::Int16Short:
  case cv::SimpleTypeKind::UInt16:
  case cv::SimpleTypeKind::UInt16Short:
  case cv::SimpleTypeKind::WideCharacter:
  case cv::SimpleTypeKind::Character16:
    return 2;

  case cv::SimpleTypeKind::Boolean32:
  case cv::SimpleTypeKind::Int32:
  case cv::SimpleTypeKind::Int32Long:
  case cv::SimpleTypeKind::UInt32:
  case cv::SimpleTypeKind::UInt32Long:
  case cv::SimpleTypeKind::Float32:
  case cv::SimpleTypeKind::Character32:
    return 4;

  case cv::SimpleTypeKind::Boolean64:
  case cv::SimpleTypeKind::Int64:
  case cv::SimpleTypeKind::Int64Quad:
  case cv::SimpleTypeKind::UInt64:
  case cv::SimpleTypeKind::UInt64Quad:
  case cv::SimpleTypeKind::Float64:
    return 8;

  case cv::SimpleTypeKind::Int128:
  case cv::SimpleTypeKind::Int128Oct:
  case cv::SimpleTypeKind::UInt128:
  case cv::SimpleTypeKind::UInt128Oct:
  case cv::SimpleTypeKind::Float128:
    return 16;

  default:
    return 0;
  }
}

/// Which register a value measured from the frame is measured from. These are
/// llvm::codeview::RegisterId, named here so the header is not needed to read
/// the comparison.
constexpr uint16_t TheFramePointer = 334;

constexpr uint16_t TheStackPointer = 335;

} // namespace

/// One PDB, read into tables.
struct PDBCReaderOpaque {
  llvm::BumpPtrAllocator Allocator;
  std::unique_ptr<llvm::MemoryBuffer> Buffer;
  std::unique_ptr<pdb::PDBFile> File;

  /// Where each section begins, by segment number less one. A CodeView record
  /// says segment and offset; an address is the two put together.
  std::vector<uint32_t> SectionAddresses;

  struct Line {
    uint32_t Address;
    uint32_t Number;
    std::string File;
  };

  /// Sorted by address, so the line an address is in is found by looking for
  /// the last one at or before it.
  std::vector<Line> Lines;

  struct Function {
    uint32_t Address;
    uint32_t Size;
    std::string Name;
  };

  std::vector<Function> Functions;

  struct Local {
    uint32_t FunctionAddress;
    std::string Name;
    int32_t FrameOffset;
    uint32_t TypeIndex;
    bool IsParameter;
    bool FromStackPointer;
  };

  std::vector<Local> Locals;

  /// The type records, by index less the first index the file uses.
  std::vector<cv::CVType> TypeRecords;
  uint32_t FirstTypeIndex = 0;

  /// Where a type that was written out in full sits, by the name it carries.
  /// A record naming a type without describing it - which is what most
  /// mentions of one are - is answered by looking the name up here.
  std::map<std::string, uint32_t> WrittenOutInFull;

  /// Text handed out, kept so it lives as long as this does. A caller is told
  /// it need free nothing, and that is what makes it true.
  std::vector<std::unique_ptr<std::string>> HandedOut;

  uint32_t AddressOfSection(uint16_t Segment) const {
    unsigned Which = Segment == 0 ? 0 : Segment - 1;

    return Which < SectionAddresses.size() ? SectionAddresses[Which] : 0;
  }

  const cv::CVType *TypeRecord(uint32_t Index) const {
    if (Index < FirstTypeIndex)
      return nullptr;

    uint32_t Where = Index - FirstTypeIndex;

    return Where < TypeRecords.size() ? &TypeRecords[Where] : nullptr;
  }

  const char *Keep(const std::string &Text) {
    HandedOut.push_back(std::make_unique<std::string>(Text));

    return HandedOut.back()->c_str();
  }

  /// The function an address is inside, or nothing.
  const Function *FunctionAt(uint32_t Address) const {
    for (const auto &One : Functions) {
      if (Address >= One.Address && Address < One.Address + One.Size)
        return &One;
    }

    return nullptr;
  }
};

namespace {

char *CopyOut(const std::string &Text) {
  char *Out = static_cast<char *>(std::malloc(Text.size() + 1));

  if (Out != nullptr)
    std::memcpy(Out, Text.c_str(), Text.size() + 1);

  return Out;
}

void SayWhy(char **ErrorMessage, const std::string &Why) {
  if (ErrorMessage != nullptr)
    *ErrorMessage = CopyOut(Why);
}

/// Reads the lines and the symbols of one module into the tables.
void ReadOneModule(PDBCReaderOpaque &Reader, pdb::ModuleDebugStreamRef &Stream,
                   const pdb::PDBStringTable *Strings) {
  // Where each file name sits in the checksums, so a line block naming an
  // offset can be turned into a name.
  std::map<uint32_t, std::string> Files;

  if (auto Checksums = Stream.findChecksumsSubsection()) {
    for (auto One = Checksums->begin(); One != Checksums->end(); ++One) {
      if (Strings == nullptr)
        break;

      if (auto Named = Strings->getStringForID(One->FileNameOffset))
        Files[One.offset()] = Named->str();
      else
        llvm::consumeError(Named.takeError());
    }
  } else {
    llvm::consumeError(Checksums.takeError());
  }

  for (const auto &Subsection : Stream.subsections()) {
    if (Subsection.kind() != cv::DebugSubsectionKind::Lines)
      continue;

    cv::DebugLinesSubsectionRef Lines;
    llvm::BinaryStreamReader Reading(Subsection.getRecordData());

    if (auto Trouble = Lines.initialize(Reading)) {
      llvm::consumeError(std::move(Trouble));
      continue;
    }

    uint32_t Base = Reader.AddressOfSection(Lines.header()->RelocSegment) +
                    Lines.header()->RelocOffset;

    for (const auto &Block : Lines) {
      auto Named = Files.find(Block.NameIndex);
      std::string File = Named != Files.end() ? Named->second : std::string();

      for (const auto &Entry : Block.LineNumbers) {
        // The line a run of code begins at is the low twenty-four bits; the
        // rest says whether it is a statement and where it ends.
        Reader.Lines.push_back(
            {Base + Entry.Offset, Entry.Flags & 0x00ffffffu, File});
      }
    }
  }

  // A function opens a scope and every scope inside it opens another, each
  // closed by its own end. The depth is counted so that the function is left
  // on the end that matches it - otherwise a value written after a nested
  // scope is read as belonging to nothing.
  uint32_t Inside = 0;
  int Depth = 0;
  bool MeasuredFromTheStackPointer = false;

  PDBCReaderOpaque::Local Pending;
  bool HavePending = false;

  bool WentWrong = false;

  for (const auto &Symbol : Stream.symbols(&WentWrong)) {
    switch (Symbol.kind()) {
    case cv::SymbolKind::S_GPROC32:
    case cv::SymbolKind::S_LPROC32: {
      if (auto Function =
              cv::SymbolDeserializer::deserializeAs<cv::ProcSym>(Symbol)) {
        Inside = Reader.AddressOfSection(Function->Segment) +
                 Function->CodeOffset;
        Depth = 1;
        MeasuredFromTheStackPointer = false;

        Reader.Functions.push_back(
            {Inside, Function->CodeSize, Function->Name.str()});
      } else {
        llvm::consumeError(Function.takeError());
      }
      break;
    }

    case cv::SymbolKind::S_FRAMEPROC: {
      // Says which register the values of this function are measured from.
      if (auto Frame =
              cv::SymbolDeserializer::deserializeAs<cv::FrameProcSym>(Symbol)) {
        MeasuredFromTheStackPointer =
            Frame->getLocalFramePtrReg(cv::CPUType::X64) ==
            cv::RegisterId::RSP;
      } else {
        llvm::consumeError(Frame.takeError());
      }
      break;
    }

    case cv::SymbolKind::S_BLOCK32:
    case cv::SymbolKind::S_INLINESITE:
      if (Depth > 0)
        Depth++;
      break;

    case cv::SymbolKind::S_END:
    case cv::SymbolKind::S_INLINESITE_END:
      if (Depth > 0)
        Depth--;
      HavePending = false;
      break;

    case cv::SymbolKind::S_LOCAL: {
      if (auto Local =
              cv::SymbolDeserializer::deserializeAs<cv::LocalSym>(Symbol)) {
        Pending = PDBCReaderOpaque::Local{};
        Pending.FunctionAddress = Inside;
        Pending.Name = Local->Name.str();
        Pending.TypeIndex = Local->Type.getIndex();
        Pending.IsParameter = (Local->Flags & cv::LocalSymFlags::IsParameter) !=
                              cv::LocalSymFlags::None;
        HavePending = true;
      } else {
        llvm::consumeError(Local.takeError());
      }
      break;
    }

    // A local says what it is called; the record after it says where it sits.
    // Neither is any use without the other, so one is held until the other
    // arrives.
    case cv::SymbolKind::S_DEFRANGE_FRAMEPOINTER_REL: {
      if (HavePending && Depth > 0) {
        if (auto Range = cv::SymbolDeserializer::deserializeAs<
                cv::DefRangeFramePointerRelSym>(Symbol)) {
          Pending.FrameOffset = Range->Hdr.Offset;
          Pending.FromStackPointer = MeasuredFromTheStackPointer;

          Reader.Locals.push_back(Pending);
        } else {
          llvm::consumeError(Range.takeError());
        }
      }
      HavePending = false;
      break;
    }

    case cv::SymbolKind::S_DEFRANGE_REGISTER_REL: {
      if (HavePending && Depth > 0) {
        if (auto Range = cv::SymbolDeserializer::deserializeAs<
                cv::DefRangeRegisterRelSym>(Symbol)) {
          uint16_t Register = Range->Hdr.Register;

          if (Register == TheFramePointer || Register == TheStackPointer) {
            Pending.FrameOffset = Range->Hdr.BasePointerOffset;
            Pending.FromStackPointer = Register == TheStackPointer;

            Reader.Locals.push_back(Pending);
          }
        } else {
          llvm::consumeError(Range.takeError());
        }
      }
      HavePending = false;
      break;
    }

    default:
      break;
    }
  }
}

} // namespace

namespace {

/// What a type is, following a mention of one through to where it was written
/// out in full.
bool Describe(PDBCReaderOpaque &Reader, uint32_t Index, PDBCTypeKind *Kind,
              const char **Name, uint32_t *SizeInBytes,
              uint32_t *PointeeTypeIndex, uint32_t Depth) {
  // A mention that leads to a mention that leads back is a file that is wrong
  // rather than a shape to follow, so the following stops.
  if (Depth > 8)
    return false;

  cv::TypeIndex Told(Index);

  if (Told.isSimple()) {
    // One of the machine's own types, said in a way that means an address to
    // one rather than one itself. Reported as an address, or it is read as a
    // number and shown as nonsense.
    if (Told.getSimpleMode() != cv::SimpleTypeMode::Direct) {
      if (Kind != nullptr)
        *Kind = PDBCTypeKindPointer;

      if (Name != nullptr)
        *Name = Reader.Keep(cv::TypeIndex::simpleTypeName(Told).str());

      if (SizeInBytes != nullptr)
        *SizeInBytes = 8;

      if (PointeeTypeIndex != nullptr)
        *PointeeTypeIndex = Told.makeDirect().getIndex();

      return true;
    }

    if (Kind != nullptr)
      *Kind = PDBCTypeKindBasic;

    if (Name != nullptr)
      *Name = Reader.Keep(cv::TypeIndex::simpleTypeName(Told).str());

    if (SizeInBytes != nullptr)
      *SizeInBytes = HowWideOneOfTheMachinesOwnIs(Told.getSimpleKind());

    return true;
  }

  const cv::CVType *Type = Reader.TypeRecord(Index);

  if (Type == nullptr)
    return false;

  switch (Type->kind()) {
  case cv::TypeLeafKind::LF_CLASS:
  case cv::TypeLeafKind::LF_STRUCTURE: {
    cv::ClassRecord Described(cv::TypeRecordKind::Class);

    if (auto Trouble = cv::TypeDeserializer::deserializeAs(
            const_cast<cv::CVType &>(*Type), Described)) {
      llvm::consumeError(std::move(Trouble));
      return false;
    }

    // A mention of a type carries no parts. Following it to where the type was
    // written out in full is what makes the parts findable.
    if (Described.isForwardRef()) {
      std::string Named = Described.hasUniqueName()
                              ? Described.getUniqueName().str()
                              : Described.getName().str();
      auto Whole = Reader.WrittenOutInFull.find(Named);

      if (Whole != Reader.WrittenOutInFull.end() && Whole->second != Index)
        return Describe(Reader, Whole->second, Kind, Name, SizeInBytes,
                        PointeeTypeIndex, Depth + 1);
    }

    if (Kind != nullptr)
      *Kind = PDBCTypeKindComposite;

    if (Name != nullptr)
      *Name = Reader.Keep(Described.getName().str());

    if (SizeInBytes != nullptr)
      *SizeInBytes = static_cast<uint32_t>(Described.getSize());

    return true;
  }

  case cv::TypeLeafKind::LF_POINTER: {
    cv::PointerRecord Described(cv::TypeRecordKind::Pointer);

    if (auto Trouble = cv::TypeDeserializer::deserializeAs(
            const_cast<cv::CVType &>(*Type), Described)) {
      llvm::consumeError(std::move(Trouble));
      return false;
    }

    if (Kind != nullptr)
      *Kind = PDBCTypeKindPointer;

    if (Name != nullptr)
      *Name = Reader.Keep(std::string());

    if (SizeInBytes != nullptr)
      *SizeInBytes = Described.getSize();

    if (PointeeTypeIndex != nullptr)
      *PointeeTypeIndex = Described.getReferentType().getIndex();

    return true;
  }

  case cv::TypeLeafKind::LF_ARRAY: {
    cv::ArrayRecord Described(cv::TypeRecordKind::Array);

    if (auto Trouble = cv::TypeDeserializer::deserializeAs(
            const_cast<cv::CVType &>(*Type), Described)) {
      llvm::consumeError(std::move(Trouble));
      return false;
    }

    if (Kind != nullptr)
      *Kind = PDBCTypeKindArray;

    if (Name != nullptr)
      *Name = Reader.Keep(Described.getName().str());

    // The whole run rather than one of them, which is what the record says.
    // How many there are is this divided by the width of what it holds, and
    // that is the caller's to work out - it has the one type index it needs.
    if (SizeInBytes != nullptr)
      *SizeInBytes = (uint32_t)Described.getSize();

    if (PointeeTypeIndex != nullptr)
      *PointeeTypeIndex = Described.getElementType().getIndex();

    return true;
  }

  case cv::TypeLeafKind::LF_ENUM: {
    cv::EnumRecord Described(cv::TypeRecordKind::Enum);

    if (auto Trouble = cv::TypeDeserializer::deserializeAs(
            const_cast<cv::CVType &>(*Type), Described)) {
      llvm::consumeError(std::move(Trouble));
      return false;
    }

    if (Kind != nullptr)
      *Kind = PDBCTypeKindEnum;

    if (Name != nullptr)
      *Name = Reader.Keep(Described.getName().str());

    if (SizeInBytes != nullptr)
      *SizeInBytes = 4;

    return true;
  }

  case cv::TypeLeafKind::LF_MODIFIER: {
    // Something said about a type rather than a type: const, volatile. What it
    // is said about is what a reader wants.
    cv::ModifierRecord Described(cv::TypeRecordKind::Modifier);

    if (auto Trouble = cv::TypeDeserializer::deserializeAs(
            const_cast<cv::CVType &>(*Type), Described)) {
      llvm::consumeError(std::move(Trouble));
      return false;
    }

    return Describe(Reader, Described.getModifiedType().getIndex(), Kind, Name,
                    SizeInBytes, PointeeTypeIndex, Depth + 1);
  }

  default:
    return false;
  }
}

/// The parts of a type, following a mention through as Describe does.
std::vector<MemberCollector::Field> FieldsOf(PDBCReaderOpaque &Reader,
                                             uint32_t Index, uint32_t Depth) {
  std::vector<MemberCollector::Field> None;

  if (Depth > 8)
    return None;

  const cv::CVType *Type = Reader.TypeRecord(Index);

  if (Type == nullptr || (Type->kind() != cv::TypeLeafKind::LF_CLASS &&
                          Type->kind() != cv::TypeLeafKind::LF_STRUCTURE))
    return None;

  cv::ClassRecord Described(cv::TypeRecordKind::Class);

  if (auto Trouble = cv::TypeDeserializer::deserializeAs(
          const_cast<cv::CVType &>(*Type), Described)) {
    llvm::consumeError(std::move(Trouble));
    return None;
  }

  if (Described.isForwardRef()) {
    std::string Named = Described.hasUniqueName()
                            ? Described.getUniqueName().str()
                            : Described.getName().str();
    auto Whole = Reader.WrittenOutInFull.find(Named);

    if (Whole != Reader.WrittenOutInFull.end() && Whole->second != Index)
      return FieldsOf(Reader, Whole->second, Depth + 1);

    return None;
  }

  const cv::CVType *List =
      Reader.TypeRecord(Described.getFieldList().getIndex());

  if (List == nullptr)
    return None;

  MemberCollector Collecting;

  if (auto Trouble = cv::visitMemberRecordStream(List->content(), Collecting)) {
    llvm::consumeError(std::move(Trouble));
    return None;
  }

  return Collecting.Fields;
}

} // namespace

extern "C" {

PDB_C_API PDBCReaderRef PDBCReaderOpen(const char *Path, char **ErrorMessage) {
  if (Path == nullptr) {
    SayWhy(ErrorMessage, "no path was given");
    return nullptr;
  }

  auto Reader = std::make_unique<PDBCReaderOpaque>();
  auto Opened = llvm::MemoryBuffer::getFile(Path, false, false);

  if (!Opened) {
    SayWhy(ErrorMessage, std::string("could not open '") + Path +
                             "': " + Opened.getError().message());
    return nullptr;
  }

  Reader->Buffer = std::move(*Opened);

  auto Bytes = llvm::ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(Reader->Buffer->getBufferStart()),
      Reader->Buffer->getBufferSize());

  Reader->File = std::make_unique<pdb::PDBFile>(
      Path,
      std::make_unique<llvm::BinaryByteStream>(Bytes, llvm::endianness::little),
      Reader->Allocator);

  if (auto Trouble = Reader->File->parseFileHeaders()) {
    llvm::consumeError(std::move(Trouble));
    SayWhy(ErrorMessage, "the file does not begin like a PDB");
    return nullptr;
  }

  if (auto Trouble = Reader->File->parseStreamData()) {
    llvm::consumeError(std::move(Trouble));
    SayWhy(ErrorMessage, "the streams inside it could not be read");
    return nullptr;
  }

  auto Dbi = Reader->File->getPDBDbiStream();

  if (!Dbi) {
    llvm::consumeError(Dbi.takeError());
    SayWhy(ErrorMessage, "it holds nothing about the program it describes");
    return nullptr;
  }

  for (const auto &Section : Dbi->getSectionHeaders())
    Reader->SectionAddresses.push_back(Section.VirtualAddress);

  auto Strings = Reader->File->getStringTable();

  if (!Strings)
    llvm::consumeError(Strings.takeError());

  const pdb::DbiModuleList &Modules = Dbi->modules();

  for (uint32_t Which = 0; Which < Modules.getModuleCount(); ++Which) {
    pdb::DbiModuleDescriptor Described = Modules.getModuleDescriptor(Which);
    uint16_t Index = Described.getModuleStreamIndex();

    // A module that contributed nothing has no stream of its own.
    if (Index == 0xffff)
      continue;

    auto Stream = Reader->File->safelyCreateIndexedStream(Index);

    if (!Stream) {
      llvm::consumeError(Stream.takeError());
      continue;
    }

    pdb::ModuleDebugStreamRef Reading(Described, std::move(*Stream));

    if (auto Trouble = Reading.reload()) {
      llvm::consumeError(std::move(Trouble));
      continue;
    }

    ReadOneModule(*Reader, Reading, Strings ? &*Strings : nullptr);
  }

  if (auto Tpi = Reader->File->getPDBTpiStream()) {
    Reader->FirstTypeIndex = Tpi->TypeIndexBegin();

    uint32_t Index = Reader->FirstTypeIndex;

    for (const auto &Type : Tpi->typeArray()) {
      Reader->TypeRecords.push_back(Type);

      if (Type.kind() == cv::TypeLeafKind::LF_CLASS ||
          Type.kind() == cv::TypeLeafKind::LF_STRUCTURE) {
        cv::ClassRecord Described(cv::TypeRecordKind::Class);
        cv::CVType Copy = Type;

        if (auto Trouble = cv::TypeDeserializer::deserializeAs(Copy, Described))
          llvm::consumeError(std::move(Trouble));
        else if (!Described.isForwardRef())
          Reader->WrittenOutInFull[Described.hasUniqueName()
                                       ? Described.getUniqueName().str()
                                       : Described.getName().str()] = Index;
      }

      Index++;
    }
  } else {
    llvm::consumeError(Tpi.takeError());
  }

  std::sort(Reader->Lines.begin(), Reader->Lines.end(),
            [](const PDBCReaderOpaque::Line &Left,
               const PDBCReaderOpaque::Line &Right) {
              return Left.Address < Right.Address;
            });

  std::sort(Reader->Functions.begin(), Reader->Functions.end(),
            [](const PDBCReaderOpaque::Function &Left,
               const PDBCReaderOpaque::Function &Right) {
              return Left.Address < Right.Address;
            });

  return Reader.release();
}

PDB_C_API void PDBCReaderClose(PDBCReaderRef Reader) { delete Reader; }

PDB_C_API void PDBCDisposeMessage(char *Message) { std::free(Message); }

PDB_C_API uint32_t PDBCAddressesOfLine(PDBCReaderRef Reader, const char *File,
                                       uint32_t Line, uint32_t *Addresses,
                                       uint32_t HowManyWanted) {
  if (Reader == nullptr || File == nullptr)
    return 0;

  std::string Wanted = LastPartLowered(File);
  uint32_t Found = 0;

  for (const auto &One : Reader->Lines) {
    if (One.Number != Line || LastPartLowered(One.File) != Wanted)
      continue;

    if (Addresses != nullptr && Found < HowManyWanted)
      Addresses[Found] = One.Address;

    Found++;
  }

  return Found;
}

PDB_C_API uint32_t PDBCLineAtAddress(PDBCReaderRef Reader, uint32_t Address,
                                     const char **File) {
  if (Reader == nullptr)
    return 0;

  // Only inside a function the program describes. The line records say where
  // each run of code begins and nothing about where the last one ends, so an
  // address past all of them would otherwise be answered with the last line in
  // the program - and a program stopped before it has run anything is stopped
  // in code somebody else wrote, nowhere near any of this. Answering that with
  // a line sends whoever asked to a file that has nothing to do with it.
  if (Reader->FunctionAt(Address) == nullptr)
    return 0;

  const PDBCReaderOpaque::Line *Best = nullptr;

  for (const auto &One : Reader->Lines) {
    if (One.Address > Address)
      break;

    Best = &One;
  }

  if (Best == nullptr)
    return 0;

  if (File != nullptr)
    *File = Reader->Keep(Best->File);

  return Best->Number;
}

PDB_C_API const char *PDBCFunctionAtAddress(PDBCReaderRef Reader,
                                            uint32_t Address) {
  if (Reader == nullptr)
    return nullptr;

  const PDBCReaderOpaque::Function *Found = Reader->FunctionAt(Address);

  return Found == nullptr ? nullptr : Reader->Keep(Found->Name);
}

PDB_C_API uint32_t PDBCFunctionStartAtAddress(PDBCReaderRef Reader,
                                              uint32_t Address) {
  if (Reader == nullptr)
    return 0;

  const PDBCReaderOpaque::Function *Found = Reader->FunctionAt(Address);

  return Found == nullptr ? 0 : Found->Address;
}

PDB_C_API uint32_t PDBCCountLocalsAtAddress(PDBCReaderRef Reader,
                                            uint32_t Address) {
  if (Reader == nullptr)
    return 0;

  const PDBCReaderOpaque::Function *Found = Reader->FunctionAt(Address);

  if (Found == nullptr)
    return 0;

  uint32_t HowMany = 0;

  for (const auto &One : Reader->Locals) {
    if (One.FunctionAddress == Found->Address)
      HowMany++;
  }

  return HowMany;
}

PDB_C_API const char *PDBCLocalAtAddress(PDBCReaderRef Reader, uint32_t Address,
                                         uint32_t Which, int32_t *FrameOffset,
                                         uint32_t *TypeIndex, int *IsParameter,
                                         int *FromStackPointer) {
  if (Reader == nullptr)
    return nullptr;

  const PDBCReaderOpaque::Function *Found = Reader->FunctionAt(Address);

  if (Found == nullptr)
    return nullptr;

  uint32_t Seen = 0;

  for (const auto &One : Reader->Locals) {
    if (One.FunctionAddress != Found->Address)
      continue;

    if (Seen++ != Which)
      continue;

    if (FrameOffset != nullptr)
      *FrameOffset = One.FrameOffset;

    if (TypeIndex != nullptr)
      *TypeIndex = One.TypeIndex;

    if (IsParameter != nullptr)
      *IsParameter = One.IsParameter ? 1 : 0;

    if (FromStackPointer != nullptr)
      *FromStackPointer = One.FromStackPointer ? 1 : 0;

    return Reader->Keep(One.Name);
  }

  return nullptr;
}

PDB_C_API int PDBCTypeInformation(PDBCReaderRef Reader, uint32_t TypeIndex,
                                  PDBCTypeKind *Kind, const char **Name,
                                  uint32_t *SizeInBytes,
                                  uint32_t *PointeeTypeIndex) {
  if (Reader == nullptr)
    return 0;

  if (Kind != nullptr)
    *Kind = PDBCTypeKindUnknown;

  if (PointeeTypeIndex != nullptr)
    *PointeeTypeIndex = 0;

  return Describe(*Reader, TypeIndex, Kind, Name, SizeInBytes, PointeeTypeIndex,
                  0)
             ? 1
             : 0;
}

PDB_C_API uint32_t PDBCCountFields(PDBCReaderRef Reader, uint32_t TypeIndex) {
  if (Reader == nullptr)
    return 0;

  return static_cast<uint32_t>(FieldsOf(*Reader, TypeIndex, 0).size());
}

PDB_C_API const char *PDBCFieldAt(PDBCReaderRef Reader, uint32_t TypeIndex,
                                  uint32_t Which, uint32_t *Offset,
                                  uint32_t *FieldTypeIndex) {
  if (Reader == nullptr)
    return nullptr;

  std::vector<MemberCollector::Field> Fields = FieldsOf(*Reader, TypeIndex, 0);

  if (Which >= Fields.size())
    return nullptr;

  if (Offset != nullptr)
    *Offset = Fields[Which].Offset;

  if (FieldTypeIndex != nullptr)
    *FieldTypeIndex = Fields[Which].TypeIndex;

  return Reader->Keep(Fields[Which].Name);
}

PDB_C_API const char *PDBCVersion(void) { return PDB_C_VERSION; }

} // extern "C"
