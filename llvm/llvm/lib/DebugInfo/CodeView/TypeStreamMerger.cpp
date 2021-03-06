//===-- TypeStreamMerger.cpp ------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/DebugInfo/CodeView/TypeStreamMerger.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/DebugInfo/CodeView/GlobalTypeTableBuilder.h"
#include "llvm/DebugInfo/CodeView/MergingTypeTableBuilder.h"
#include "llvm/DebugInfo/CodeView/TypeIndex.h"
#include "llvm/DebugInfo/CodeView/TypeIndexDiscovery.h"
#include "llvm/DebugInfo/CodeView/TypeRecord.h"
#include "llvm/Support/Error.h"

using namespace llvm;
using namespace llvm::codeview;

namespace {

/// Implementation of CodeView type stream merging.
///
/// A CodeView type stream is a series of records that reference each other
/// through type indices. A type index is either "simple", meaning it is less
/// than 0x1000 and refers to a builtin type, or it is complex, meaning it
/// refers to a prior type record in the current stream. The type index of a
/// record is equal to the number of records before it in the stream plus
/// 0x1000.
///
/// Type records are only allowed to use type indices smaller than their own, so
/// a type stream is effectively a topologically sorted DAG. Cycles occuring in
/// the type graph of the source program are resolved with forward declarations
/// of composite types. This class implements the following type stream merging
/// algorithm, which relies on this DAG structure:
///
/// - Begin with a new empty stream, and a new empty hash table that maps from
///   type record contents to new type index.
/// - For each new type stream, maintain a map from source type index to
///   destination type index.
/// - For each record, copy it and rewrite its type indices to be valid in the
///   destination type stream.
/// - If the new type record is not already present in the destination stream
///   hash table, append it to the destination type stream, assign it the next
///   type index, and update the two hash tables.
/// - If the type record already exists in the destination stream, discard it
///   and update the type index map to forward the source type index to the
///   existing destination type index.
///
/// As an additional complication, type stream merging actually produces two
/// streams: an item (or IPI) stream and a type stream, as this is what is
/// actually stored in the final PDB. We choose which records go where by
/// looking at the record kind.
class TypeStreamMerger {
public:
  explicit TypeStreamMerger(SmallVectorImpl<TypeIndex> &SourceToDest)
      : IndexMap(SourceToDest) {
    SourceToDest.clear();
  }

  static const TypeIndex Untranslated;

  // Local hashing entry points
  Error mergeTypesAndIds(MergingTypeTableBuilder &DestIds,
                         MergingTypeTableBuilder &DestTypes,
                         const CVTypeArray &IdsAndTypes);
  Error mergeIdRecords(MergingTypeTableBuilder &Dest,
                       ArrayRef<TypeIndex> TypeSourceToDest,
                       const CVTypeArray &Ids);
  Error mergeTypeRecords(MergingTypeTableBuilder &Dest,
                         const CVTypeArray &Types);

  // Global hashing entry points
  Error mergeTypesAndIds(GlobalTypeTableBuilder &DestIds,
                         GlobalTypeTableBuilder &DestTypes,
                         const CVTypeArray &IdsAndTypes,
                         ArrayRef<GloballyHashedType> Hashes);
  Error mergeIdRecords(GlobalTypeTableBuilder &Dest,
                       ArrayRef<TypeIndex> TypeSourceToDest,
                       const CVTypeArray &Ids,
                       ArrayRef<GloballyHashedType> Hashes);
  Error mergeTypeRecords(GlobalTypeTableBuilder &Dest, const CVTypeArray &Types,
                         ArrayRef<GloballyHashedType> Hashes);

private:
  Error doit(const CVTypeArray &Types);

  Error remapAllTypes(const CVTypeArray &Types);

  Error remapType(const CVType &Type);

  void addMapping(TypeIndex Idx);

  bool remapTypeIndex(TypeIndex &Idx);
  bool remapItemIndex(TypeIndex &Idx);

  bool hasTypeStream() const {
    return (UseGlobalHashes) ? (!!DestGlobalTypeStream) : (!!DestTypeStream);
  }

  bool hasIdStream() const {
    return (UseGlobalHashes) ? (!!DestGlobalIdStream) : (!!DestIdStream);
  }

  ArrayRef<uint8_t> serializeRemapped(const RemappedType &Record);

  bool remapIndices(RemappedType &Record, ArrayRef<TiReference> Refs);

  bool remapIndex(TypeIndex &Idx, ArrayRef<TypeIndex> Map);

  size_t slotForIndex(TypeIndex Idx) const {
    assert(!Idx.isSimple() && "simple type indices have no slots");
    return Idx.getIndex() - TypeIndex::FirstNonSimpleIndex;
  }

  Error errorCorruptRecord() const {
    return llvm::make_error<CodeViewError>(cv_error_code::corrupt_record);
  }

  Optional<Error> LastError;

  bool UseGlobalHashes = false;

  bool IsSecondPass = false;

  unsigned NumBadIndices = 0;

  TypeIndex CurIndex{TypeIndex::FirstNonSimpleIndex};

  MergingTypeTableBuilder *DestIdStream = nullptr;
  MergingTypeTableBuilder *DestTypeStream = nullptr;

  GlobalTypeTableBuilder *DestGlobalIdStream = nullptr;
  GlobalTypeTableBuilder *DestGlobalTypeStream = nullptr;

  ArrayRef<GloballyHashedType> GlobalHashes;

  // If we're only mapping id records, this array contains the mapping for
  // type records.
  ArrayRef<TypeIndex> TypeLookup;

  /// Map from source type index to destination type index. Indexed by source
  /// type index minus 0x1000.
  SmallVectorImpl<TypeIndex> &IndexMap;

  /// Temporary storage that we use to copy a record's data while re-writing
  /// its type indices.
  SmallVector<uint8_t, 256> RemapStorage;
};

} // end anonymous namespace

ArrayRef<uint8_t>
TypeStreamMerger::serializeRemapped(const RemappedType &Record) {
  TypeIndex TI;
  ArrayRef<uint8_t> OriginalData = Record.OriginalRecord.RecordData;
  if (Record.Mappings.empty())
    return OriginalData;

  // At least one type index was remapped.  We copy the full record bytes,
  // re-write each type index, then return that.
  RemapStorage.resize(OriginalData.size());
  ::memcpy(&RemapStorage[0], OriginalData.data(), OriginalData.size());
  uint8_t *ContentBegin = RemapStorage.data() + sizeof(RecordPrefix);
  for (const auto &M : Record.Mappings) {
    // First 4 bytes of every record are the record prefix, but the mapping
    // offset is relative to the content which starts after.
    *(TypeIndex *)(ContentBegin + M.first) = M.second;
  }
  auto RemapRef = makeArrayRef(RemapStorage);
  return RemapRef;
}

const TypeIndex TypeStreamMerger::Untranslated(SimpleTypeKind::NotTranslated);

static bool isIdRecord(TypeLeafKind K) {
  switch (K) {
  case TypeLeafKind::LF_FUNC_ID:
  case TypeLeafKind::LF_MFUNC_ID:
  case TypeLeafKind::LF_STRING_ID:
  case TypeLeafKind::LF_SUBSTR_LIST:
  case TypeLeafKind::LF_BUILDINFO:
  case TypeLeafKind::LF_UDT_SRC_LINE:
  case TypeLeafKind::LF_UDT_MOD_SRC_LINE:
    return true;
  default:
    return false;
  }
}

void TypeStreamMerger::addMapping(TypeIndex Idx) {
  if (!IsSecondPass) {
    assert(IndexMap.size() == slotForIndex(CurIndex) &&
           "visitKnownRecord should add one index map entry");
    IndexMap.push_back(Idx);
  } else {
    assert(slotForIndex(CurIndex) < IndexMap.size());
    IndexMap[slotForIndex(CurIndex)] = Idx;
  }
}

bool TypeStreamMerger::remapIndex(TypeIndex &Idx, ArrayRef<TypeIndex> Map) {
  // Simple types are unchanged.
  if (Idx.isSimple())
    return true;

  // Check if this type index refers to a record we've already translated
  // successfully. If it refers to a type later in the stream or a record we
  // had to defer, defer it until later pass.
  unsigned MapPos = slotForIndex(Idx);
  if (MapPos < Map.size() && Map[MapPos] != Untranslated) {
    Idx = Map[MapPos];
    return true;
  }

  // If this is the second pass and this index isn't in the map, then it points
  // outside the current type stream, and this is a corrupt record.
  if (IsSecondPass && MapPos >= Map.size()) {
    // FIXME: Print a more useful error. We can give the current record and the
    // index that we think its pointing to.
    LastError = joinErrors(std::move(*LastError), errorCorruptRecord());
  }

  ++NumBadIndices;

  // This type index is invalid. Remap this to "not translated by cvpack",
  // and return failure.
  Idx = Untranslated;
  return false;
}

bool TypeStreamMerger::remapTypeIndex(TypeIndex &Idx) {
  // If we're mapping a pure index stream, then IndexMap only contains mappings
  // from OldIdStream -> NewIdStream, in which case we will need to use the
  // special mapping from OldTypeStream -> NewTypeStream which was computed
  // externally.  Regardless, we use this special map if and only if we are
  // doing an id-only mapping.
  if (!hasTypeStream())
    return remapIndex(Idx, TypeLookup);

  assert(TypeLookup.empty());
  return remapIndex(Idx, IndexMap);
}

bool TypeStreamMerger::remapItemIndex(TypeIndex &Idx) {
  assert(hasIdStream());
  return remapIndex(Idx, IndexMap);
}

// Local hashing entry points
Error TypeStreamMerger::mergeTypeRecords(MergingTypeTableBuilder &Dest,
                                         const CVTypeArray &Types) {
  DestTypeStream = &Dest;
  UseGlobalHashes = false;

  return doit(Types);
}

Error TypeStreamMerger::mergeIdRecords(MergingTypeTableBuilder &Dest,
                                       ArrayRef<TypeIndex> TypeSourceToDest,
                                       const CVTypeArray &Ids) {
  DestIdStream = &Dest;
  TypeLookup = TypeSourceToDest;
  UseGlobalHashes = false;

  return doit(Ids);
}

Error TypeStreamMerger::mergeTypesAndIds(MergingTypeTableBuilder &DestIds,
                                         MergingTypeTableBuilder &DestTypes,
                                         const CVTypeArray &IdsAndTypes) {
  DestIdStream = &DestIds;
  DestTypeStream = &DestTypes;
  UseGlobalHashes = false;
  return doit(IdsAndTypes);
}

// Global hashing entry points
Error TypeStreamMerger::mergeTypeRecords(GlobalTypeTableBuilder &Dest,
                                         const CVTypeArray &Types,
                                         ArrayRef<GloballyHashedType> Hashes) {
  DestGlobalTypeStream = &Dest;
  UseGlobalHashes = true;
  GlobalHashes = Hashes;

  return doit(Types);
}

Error TypeStreamMerger::mergeIdRecords(GlobalTypeTableBuilder &Dest,
                                       ArrayRef<TypeIndex> TypeSourceToDest,
                                       const CVTypeArray &Ids,
                                       ArrayRef<GloballyHashedType> Hashes) {
  DestGlobalIdStream = &Dest;
  TypeLookup = TypeSourceToDest;
  UseGlobalHashes = true;
  GlobalHashes = Hashes;

  return doit(Ids);
}

Error TypeStreamMerger::mergeTypesAndIds(GlobalTypeTableBuilder &DestIds,
                                         GlobalTypeTableBuilder &DestTypes,
                                         const CVTypeArray &IdsAndTypes,
                                         ArrayRef<GloballyHashedType> Hashes) {
  DestGlobalIdStream = &DestIds;
  DestGlobalTypeStream = &DestTypes;
  UseGlobalHashes = true;
  GlobalHashes = Hashes;
  return doit(IdsAndTypes);
}

Error TypeStreamMerger::doit(const CVTypeArray &Types) {
  if (auto EC = remapAllTypes(Types))
    return EC;

  // If we found bad indices but no other errors, try doing another pass and see
  // if we can resolve the indices that weren't in the map on the first pass.
  // This may require multiple passes, but we should always make progress. MASM
  // is the only known CodeView producer that makes type streams that aren't
  // topologically sorted. The standard library contains MASM-produced objects,
  // so this is important to handle correctly, but we don't have to be too
  // efficient. MASM type streams are usually very small.
  while (!LastError && NumBadIndices > 0) {
    unsigned BadIndicesRemaining = NumBadIndices;
    IsSecondPass = true;
    NumBadIndices = 0;
    CurIndex = TypeIndex(TypeIndex::FirstNonSimpleIndex);

    if (auto EC = remapAllTypes(Types))
      return EC;

    assert(NumBadIndices <= BadIndicesRemaining &&
           "second pass found more bad indices");
    if (!LastError && NumBadIndices == BadIndicesRemaining) {
      return llvm::make_error<CodeViewError>(
          cv_error_code::corrupt_record, "input type graph contains cycles");
    }
  }

  if (LastError)
    return std::move(*LastError);
  return Error::success();
}

Error TypeStreamMerger::remapAllTypes(const CVTypeArray &Types) {
  BinaryStreamRef Stream = Types.getUnderlyingStream();
  ArrayRef<uint8_t> Buffer;
  cantFail(Stream.readBytes(0, Stream.getLength(), Buffer));

  return forEachCodeViewRecord<CVType>(
      Buffer, [this](const CVType &T) { return remapType(T); });
}

Error TypeStreamMerger::remapType(const CVType &Type) {
  auto DoSerialize = [this, Type]() -> ArrayRef<uint8_t> {
    RemappedType R(Type);
    SmallVector<TiReference, 32> Refs;
    discoverTypeIndices(Type.RecordData, Refs);
    if (!remapIndices(R, Refs))
      return {};
    return serializeRemapped(R);
  };

  TypeIndex DestIdx = Untranslated;
  if (UseGlobalHashes) {
    GlobalTypeTableBuilder &Dest =
        isIdRecord(Type.kind()) ? *DestGlobalIdStream : *DestGlobalTypeStream;
    GloballyHashedType H = GlobalHashes[CurIndex.toArrayIndex()];
    DestIdx = Dest.insertRecordAs(H, DoSerialize);
  } else {
    MergingTypeTableBuilder &Dest =
        isIdRecord(Type.kind()) ? *DestIdStream : *DestTypeStream;

    auto Data = DoSerialize();
    if (!Data.empty())
      DestIdx = Dest.insertRecordBytes(Data);
  }
  addMapping(DestIdx);

  ++CurIndex;
  assert((IsSecondPass || IndexMap.size() == slotForIndex(CurIndex)) &&
         "visitKnownRecord should add one index map entry");
  return Error::success();
}

bool TypeStreamMerger::remapIndices(RemappedType &Record,
                                    ArrayRef<TiReference> Refs) {
  ArrayRef<uint8_t> OriginalData = Record.OriginalRecord.content();
  bool Success = true;
  for (auto &Ref : Refs) {
    uint32_t Offset = Ref.Offset;
    ArrayRef<uint8_t> Bytes = OriginalData.slice(Ref.Offset, sizeof(TypeIndex));
    ArrayRef<TypeIndex> TIs(reinterpret_cast<const TypeIndex *>(Bytes.data()),
                            Ref.Count);
    for (auto TI : TIs) {
      TypeIndex NewTI = TI;
      bool ThisSuccess = (Ref.Kind == TiRefKind::IndexRef)
                             ? remapItemIndex(NewTI)
                             : remapTypeIndex(NewTI);
      if (ThisSuccess && NewTI != TI)
        Record.Mappings.emplace_back(Offset, NewTI);
      Offset += sizeof(TypeIndex);
      Success &= ThisSuccess;
    }
  }
  return Success;
}

Error llvm::codeview::mergeTypeRecords(MergingTypeTableBuilder &Dest,
                                       SmallVectorImpl<TypeIndex> &SourceToDest,
                                       const CVTypeArray &Types) {
  TypeStreamMerger M(SourceToDest);
  return M.mergeTypeRecords(Dest, Types);
}

Error llvm::codeview::mergeIdRecords(MergingTypeTableBuilder &Dest,
                                     ArrayRef<TypeIndex> TypeSourceToDest,
                                     SmallVectorImpl<TypeIndex> &SourceToDest,
                                     const CVTypeArray &Ids) {
  TypeStreamMerger M(SourceToDest);
  return M.mergeIdRecords(Dest, TypeSourceToDest, Ids);
}

Error llvm::codeview::mergeTypeAndIdRecords(
    MergingTypeTableBuilder &DestIds, MergingTypeTableBuilder &DestTypes,
    SmallVectorImpl<TypeIndex> &SourceToDest, const CVTypeArray &IdsAndTypes) {
  TypeStreamMerger M(SourceToDest);
  return M.mergeTypesAndIds(DestIds, DestTypes, IdsAndTypes);
}

Error llvm::codeview::mergeTypeAndIdRecords(
    GlobalTypeTableBuilder &DestIds, GlobalTypeTableBuilder &DestTypes,
    SmallVectorImpl<TypeIndex> &SourceToDest, const CVTypeArray &IdsAndTypes,
    ArrayRef<GloballyHashedType> Hashes) {
  TypeStreamMerger M(SourceToDest);
  return M.mergeTypesAndIds(DestIds, DestTypes, IdsAndTypes, Hashes);
}

Error llvm::codeview::mergeTypeRecords(GlobalTypeTableBuilder &Dest,
                                       SmallVectorImpl<TypeIndex> &SourceToDest,
                                       const CVTypeArray &Types,
                                       ArrayRef<GloballyHashedType> Hashes) {
  TypeStreamMerger M(SourceToDest);
  return M.mergeTypeRecords(Dest, Types, Hashes);
}

Error llvm::codeview::mergeIdRecords(GlobalTypeTableBuilder &Dest,
                                     ArrayRef<TypeIndex> Types,
                                     SmallVectorImpl<TypeIndex> &SourceToDest,
                                     const CVTypeArray &Ids,
                                     ArrayRef<GloballyHashedType> Hashes) {
  TypeStreamMerger M(SourceToDest);
  return M.mergeIdRecords(Dest, Types, Ids, Hashes);
}
