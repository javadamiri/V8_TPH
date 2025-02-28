// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/snapshot/serializer.h"

#include "src/codegen/assembler-inl.h"
#include "src/common/globals.h"
#include "src/heap/heap-inl.h"  // For Space::identity().
#include "src/heap/memory-chunk-inl.h"
#include "src/heap/read-only-heap.h"
#include "src/interpreter/interpreter.h"
#include "src/objects/code.h"
#include "src/objects/js-array-buffer-inl.h"
#include "src/objects/js-array-inl.h"
#include "src/objects/map.h"
#include "src/objects/slots-inl.h"
#include "src/objects/smi.h"

namespace v8 {
namespace internal {

Serializer::Serializer(Isolate* isolate, Snapshot::SerializerFlags flags)
    : isolate_(isolate),
      external_reference_encoder_(isolate),
      root_index_map_(isolate),
      flags_(flags),
      allocator_(this) {
#ifdef OBJECT_PRINT
  if (FLAG_serialization_statistics) {
    for (int space = 0; space < kNumberOfSpaces; ++space) {
      // Value-initialized to 0.
      instance_type_count_[space] = std::make_unique<int[]>(kInstanceTypes);
      instance_type_size_[space] = std::make_unique<size_t[]>(kInstanceTypes);
    }
  }
#endif  // OBJECT_PRINT
}

#ifdef OBJECT_PRINT
void Serializer::CountInstanceType(Map map, int size, SnapshotSpace space) {
  const int space_number = static_cast<int>(space);
  int instance_type = map.instance_type();
  instance_type_count_[space_number][instance_type]++;
  instance_type_size_[space_number][instance_type] += size;
}
#endif  // OBJECT_PRINT

void Serializer::OutputStatistics(const char* name) {
  if (!FLAG_serialization_statistics) return;

  PrintF("%s:\n", name);
  allocator()->OutputStatistics();

#ifdef OBJECT_PRINT
  PrintF("  Instance types (count and bytes):\n");
#define PRINT_INSTANCE_TYPE(Name)                                          \
  for (int space = 0; space < kNumberOfSpaces; ++space) {                  \
    if (instance_type_count_[space][Name]) {                               \
      PrintF("%10d %10zu  %-10s %s\n", instance_type_count_[space][Name],  \
             instance_type_size_[space][Name],                             \
             BaseSpace::GetSpaceName(static_cast<AllocationSpace>(space)), \
             #Name);                                                       \
    }                                                                      \
  }
  INSTANCE_TYPE_LIST(PRINT_INSTANCE_TYPE)
#undef PRINT_INSTANCE_TYPE

  PrintF("\n");
#endif  // OBJECT_PRINT
}

void Serializer::SerializeDeferredObjects() {
  if (FLAG_trace_serializer) {
    PrintF("Serializing deferred objects\n");
  }
  while (!deferred_objects_.empty()) {
    HeapObject obj = deferred_objects_.back();
    deferred_objects_.pop_back();
    ObjectSerializer obj_serializer(this, obj, &sink_);
    obj_serializer.SerializeDeferred();
  }
  sink_.Put(kSynchronize, "Finished with deferred objects");
}

bool Serializer::MustBeDeferred(HeapObject object) { return false; }

void Serializer::VisitRootPointers(Root root, const char* description,
                                   FullObjectSlot start, FullObjectSlot end) {
  for (FullObjectSlot current = start; current < end; ++current) {
    SerializeRootObject(current);
  }
}

void Serializer::SerializeRootObject(FullObjectSlot slot) {
  Object o = *slot;
  if (o.IsSmi()) {
    PutSmiRoot(slot);
  } else {
    SerializeObject(HeapObject::cast(o));
  }
}

#ifdef DEBUG
void Serializer::PrintStack() { PrintStack(std::cout); }

void Serializer::PrintStack(std::ostream& out) {
  for (const auto o : stack_) {
    o.Print(out);
    out << "\n";
  }
}
#endif  // DEBUG

bool Serializer::SerializeRoot(HeapObject obj) {
  RootIndex root_index;
  // Derived serializers are responsible for determining if the root has
  // actually been serialized before calling this.
  if (root_index_map()->Lookup(obj, &root_index)) {
    PutRoot(root_index, obj);
    return true;
  }
  return false;
}

bool Serializer::SerializeHotObject(HeapObject obj) {
  // Encode a reference to a hot object by its index in the working set.
  int index = hot_objects_.Find(obj);
  if (index == HotObjectsList::kNotFound) return false;
  DCHECK(index >= 0 && index < kHotObjectCount);
  if (FLAG_trace_serializer) {
    PrintF(" Encoding hot object %d:", index);
    obj.ShortPrint();
    PrintF("\n");
  }
  sink_.Put(HotObject::Encode(index), "HotObject");
  return true;
}

bool Serializer::SerializeBackReference(HeapObject obj) {
  SerializerReference reference =
      reference_map_.LookupReference(reinterpret_cast<void*>(obj.ptr()));
  if (!reference.is_valid()) return false;
  // Encode the location of an already deserialized object in order to write
  // its location into a later object.  We can encode the location as an
  // offset fromthe start of the deserialized objects or as an offset
  // backwards from thecurrent allocation pointer.
  if (reference.is_attached_reference()) {
    if (FLAG_trace_serializer) {
      PrintF(" Encoding attached reference %d\n",
             reference.attached_reference_index());
    }
    PutAttachedReference(reference);
  } else {
    DCHECK(reference.is_back_reference());
    if (FLAG_trace_serializer) {
      PrintF(" Encoding back reference to: ");
      obj.ShortPrint();
      PrintF("\n");
    }

    PutAlignmentPrefix(obj);
    SnapshotSpace space = reference.space();
    sink_.Put(BackRef::Encode(space), "BackRef");
    PutBackReference(obj, reference);
  }
  return true;
}

bool Serializer::SerializePendingObject(HeapObject obj) {
  PendingObjectReference pending_obj =
      forward_refs_per_pending_object_.find(obj);
  if (pending_obj == forward_refs_per_pending_object_.end()) {
    return false;
  }

  PutPendingForwardReferenceTo(pending_obj);
  return true;
}

bool Serializer::ObjectIsBytecodeHandler(HeapObject obj) const {
  if (!obj.IsCode()) return false;
  return (Code::cast(obj).kind() == CodeKind::BYTECODE_HANDLER);
}

void Serializer::PutRoot(RootIndex root, HeapObject object) {
  int root_index = static_cast<int>(root);
  if (FLAG_trace_serializer) {
    PrintF(" Encoding root %d:", root_index);
    object.ShortPrint();
    PrintF("\n");
  }

  // Assert that the first 32 root array items are a conscious choice. They are
  // chosen so that the most common ones can be encoded more efficiently.
  STATIC_ASSERT(static_cast<int>(RootIndex::kArgumentsMarker) ==
                kRootArrayConstantsCount - 1);

  // TODO(ulan): Check that it works with young large objects.
  if (root_index < kRootArrayConstantsCount &&
       (V8_ENABLE_THIRD_PARTY_HEAP_BOOL ||
        !Heap::InYoungGeneration(object))) {
    sink_.Put(RootArrayConstant::Encode(root), "RootConstant");
  } else {
    sink_.Put(kRootArray, "RootSerialization");
    sink_.PutInt(root_index, "root_index");
    hot_objects_.Add(object);
  }
}

void Serializer::PutSmiRoot(FullObjectSlot slot) {
  // Serializing a smi root in compressed pointer builds will serialize the
  // full object slot (of kSystemPointerSize) to avoid complications during
  // deserialization (endianness or smi sequences).
  STATIC_ASSERT(decltype(slot)::kSlotDataSize == sizeof(Address));
  STATIC_ASSERT(decltype(slot)::kSlotDataSize == kSystemPointerSize);
  static constexpr int bytes_to_output = decltype(slot)::kSlotDataSize;
  static constexpr int size_in_tagged = bytes_to_output >> kTaggedSizeLog2;
  sink_.Put(FixedRawDataWithSize::Encode(size_in_tagged), "Smi");

  Address raw_value = Smi::cast(*slot).ptr();
  const byte* raw_value_as_bytes = reinterpret_cast<const byte*>(&raw_value);
  sink_.PutRaw(raw_value_as_bytes, bytes_to_output, "Bytes");
}

void Serializer::PutBackReference(HeapObject object,
                                  SerializerReference reference) {
  DCHECK(allocator()->BackReferenceIsAlreadyAllocated(reference));
  switch (reference.space()) {
    case SnapshotSpace::kMap:
      sink_.PutInt(reference.map_index(), "BackRefMapIndex");
      break;

    case SnapshotSpace::kLargeObject:
      sink_.PutInt(reference.large_object_index(), "BackRefLargeObjectIndex");
      break;

    default:
      sink_.PutInt(reference.chunk_index(), "BackRefChunkIndex");
      sink_.PutInt(reference.chunk_offset(), "BackRefChunkOffset");
      break;
  }

  hot_objects_.Add(object);
}

void Serializer::PutAttachedReference(SerializerReference reference) {
  DCHECK(reference.is_attached_reference());
  sink_.Put(kAttachedReference, "AttachedRef");
  sink_.PutInt(reference.attached_reference_index(), "AttachedRefIndex");
}

int Serializer::PutAlignmentPrefix(HeapObject object) {
  AllocationAlignment alignment = HeapObject::RequiredAlignment(object.map());
  if (alignment != kWordAligned) {
    DCHECK(1 <= alignment && alignment <= 3);
    byte prefix = (kAlignmentPrefix - 1) + alignment;
    sink_.Put(prefix, "Alignment");
    return Heap::GetMaximumFillToAlign(alignment);
  }
  return 0;
}

void Serializer::PutNextChunk(SnapshotSpace space) {
  sink_.Put(kNextChunk, "NextChunk");
  sink_.Put(static_cast<byte>(space), "NextChunkSpace");
}

void Serializer::PutRepeat(int repeat_count) {
  if (repeat_count <= kLastEncodableFixedRepeatCount) {
    sink_.Put(FixedRepeatWithCount::Encode(repeat_count), "FixedRepeat");
  } else {
    sink_.Put(kVariableRepeat, "VariableRepeat");
    sink_.PutInt(VariableRepeatCount::Encode(repeat_count), "repeat count");
  }
}

void Serializer::PutPendingForwardReferenceTo(
    PendingObjectReference reference) {
  sink_.Put(kRegisterPendingForwardRef, "RegisterPendingForwardRef");
  unresolved_forward_refs_++;
  // Register the current slot with the pending object.
  int forward_ref_id = next_forward_ref_id_++;
  reference->second.push_back(forward_ref_id);
}

void Serializer::ResolvePendingForwardReference(int forward_reference_id) {
  sink_.Put(kResolvePendingForwardRef, "ResolvePendingForwardRef");
  sink_.PutInt(forward_reference_id, "with this index");
  unresolved_forward_refs_--;

  // If there are no more unresolved forward refs, reset the forward ref id to
  // zero so that future forward refs compress better.
  if (unresolved_forward_refs_ == 0) {
    next_forward_ref_id_ = 0;
  }
}

Serializer::PendingObjectReference Serializer::RegisterObjectIsPending(
    HeapObject obj) {
  // Add the given object to the pending objects -> forward refs map.
  auto forward_refs_entry_insertion =
      forward_refs_per_pending_object_.emplace(obj, std::vector<int>());

  // If the above emplace didn't actually add the object, then the object must
  // already have been registered pending by deferring. It might not be in the
  // deferred objects queue though, since it may be the very object we just
  // popped off that queue, so just check that it can be deferred.
  DCHECK_IMPLIES(!forward_refs_entry_insertion.second, CanBeDeferred(obj));

  // return the iterator into the map as the reference.
  return forward_refs_entry_insertion.first;
}

void Serializer::ResolvePendingObject(Serializer::PendingObjectReference ref) {
  for (int index : ref->second) {
    ResolvePendingForwardReference(index);
  }
  forward_refs_per_pending_object_.erase(ref);
}

void Serializer::Pad(int padding_offset) {
  // The non-branching GetInt will read up to 3 bytes too far, so we need
  // to pad the snapshot to make sure we don't read over the end.
  for (unsigned i = 0; i < sizeof(int32_t) - 1; i++) {
    sink_.Put(kNop, "Padding");
  }
  // Pad up to pointer size for checksum.
  while (!IsAligned(sink_.Position() + padding_offset, kPointerAlignment)) {
    sink_.Put(kNop, "Padding");
  }
}

void Serializer::InitializeCodeAddressMap() {
  isolate_->InitializeLoggingAndCounters();
  code_address_map_ = std::make_unique<CodeAddressMap>(isolate_);
}

Code Serializer::CopyCode(Code code) {
  code_buffer_.clear();  // Clear buffer without deleting backing store.
  int size = code.CodeSize();
  code_buffer_.insert(code_buffer_.end(),
                      reinterpret_cast<byte*>(code.address()),
                      reinterpret_cast<byte*>(code.address() + size));
  // When pointer compression is enabled the checked cast will try to
  // decompress map field of off-heap Code object.
  return Code::unchecked_cast(HeapObject::FromAddress(
      reinterpret_cast<Address>(&code_buffer_.front())));
}

void Serializer::ObjectSerializer::SerializePrologue(SnapshotSpace space,
                                                     int size, Map map) {
  if (serializer_->code_address_map_) {
    const char* code_name =
        serializer_->code_address_map_->Lookup(object_.address());
    LOG(serializer_->isolate_,
        CodeNameEvent(object_.address(), sink_->Position(), code_name));
  }

  SerializerReference back_reference;
  if (map == object_) {
    DCHECK_EQ(object_, ReadOnlyRoots(serializer_->isolate()).meta_map());
    DCHECK_EQ(space, SnapshotSpace::kReadOnlyHeap);
    sink_->Put(kNewMetaMap, "NewMetaMap");

    DCHECK_EQ(size, Map::kSize);
    back_reference = serializer_->allocator()->Allocate(space, size);
  } else {
    sink_->Put(NewObject::Encode(space), "NewObject");

    // TODO(leszeks): Skip this when the map has a fixed size.
    sink_->PutInt(size >> kObjectAlignmentBits, "ObjectSizeInWords");

    // Until the space for the object is allocated, it is considered "pending".
    auto pending_object_ref = serializer_->RegisterObjectIsPending(object_);

    // Serialize map (first word of the object) before anything else, so that
    // the deserializer can access it when allocating. Make sure that the map
    // isn't a pending object.
    DCHECK_EQ(serializer_->forward_refs_per_pending_object_.count(map), 0);
    DCHECK(map.IsMap());
    serializer_->SerializeObject(map);

    // Make sure the map serialization didn't accidentally recursively serialize
    // this object.
    DCHECK(!serializer_->reference_map()
                ->LookupReference(reinterpret_cast<void*>(object_.ptr()))
                .is_valid());

    // Allocate the object after the map is serialized.
    if (space == SnapshotSpace::kLargeObject) {
      CHECK(!object_.IsCode());
      back_reference = serializer_->allocator()->AllocateLargeObject(size);
    } else if (space == SnapshotSpace::kMap) {
      back_reference = serializer_->allocator()->AllocateMap();
      DCHECK_EQ(Map::kSize, size);
    } else {
      int fill = serializer_->PutAlignmentPrefix(object_);
      back_reference = serializer_->allocator()->Allocate(space, size + fill);
    }

    // Now that the object is allocated, we can resolve pending references to
    // it.
    serializer_->ResolvePendingObject(pending_object_ref);
  }

#ifdef OBJECT_PRINT
  if (FLAG_serialization_statistics) {
    serializer_->CountInstanceType(map, size, space);
  }
#endif  // OBJECT_PRINT

  // Mark this object as already serialized.
  serializer_->reference_map()->Add(reinterpret_cast<void*>(object_.ptr()),
                                    back_reference);
}

uint32_t Serializer::ObjectSerializer::SerializeBackingStore(
    void* backing_store, int32_t byte_length) {
  SerializerReference reference =
      serializer_->reference_map()->LookupReference(backing_store);

  // Serialize the off-heap backing store.
  if (!reference.is_valid()) {
    sink_->Put(kOffHeapBackingStore, "Off-heap backing store");
    sink_->PutInt(byte_length, "length");
    sink_->PutRaw(static_cast<byte*>(backing_store), byte_length,
                  "BackingStore");
    reference = serializer_->allocator()->AllocateOffHeapBackingStore();
    // Mark this backing store as already serialized.
    serializer_->reference_map()->Add(backing_store, reference);
  }

  return reference.off_heap_backing_store_index();
}

void Serializer::ObjectSerializer::SerializeJSTypedArray() {
  JSTypedArray typed_array = JSTypedArray::cast(object_);
  if (typed_array.is_on_heap()) {
    typed_array.RemoveExternalPointerCompensationForSerialization(
        serializer_->isolate());
  } else {
    if (!typed_array.WasDetached()) {
      // Explicitly serialize the backing store now.
      JSArrayBuffer buffer = JSArrayBuffer::cast(typed_array.buffer());
      // We cannot store byte_length larger than int32 range in the snapshot.
      CHECK_LE(buffer.byte_length(), std::numeric_limits<int32_t>::max());
      int32_t byte_length = static_cast<int32_t>(buffer.byte_length());
      size_t byte_offset = typed_array.byte_offset();

      // We need to calculate the backing store from the data pointer
      // because the ArrayBuffer may already have been serialized.
      void* backing_store = reinterpret_cast<void*>(
          reinterpret_cast<Address>(typed_array.DataPtr()) - byte_offset);

      uint32_t ref = SerializeBackingStore(backing_store, byte_length);
      typed_array.SetExternalBackingStoreRefForSerialization(ref);
    } else {
      typed_array.SetExternalBackingStoreRefForSerialization(0);
    }
  }
  SerializeObject();
}

void Serializer::ObjectSerializer::SerializeJSArrayBuffer() {
  JSArrayBuffer buffer = JSArrayBuffer::cast(object_);
  void* backing_store = buffer.backing_store();
  // We cannot store byte_length larger than int32 range in the snapshot.
  CHECK_LE(buffer.byte_length(), std::numeric_limits<int32_t>::max());
  int32_t byte_length = static_cast<int32_t>(buffer.byte_length());
  ArrayBufferExtension* extension = buffer.extension();

  // The embedder-allocated backing store only exists for the off-heap case.
#ifdef V8_HEAP_SANDBOX
  uint32_t external_pointer_entry =
      buffer.GetBackingStoreRefForDeserialization();
#endif
  if (backing_store != nullptr) {
    uint32_t ref = SerializeBackingStore(backing_store, byte_length);
    buffer.SetBackingStoreRefForSerialization(ref);

    // Ensure deterministic output by setting extension to null during
    // serialization.
    buffer.set_extension(nullptr);
  } else {
    buffer.SetBackingStoreRefForSerialization(kNullRefSentinel);
  }

  SerializeObject();

#ifdef V8_HEAP_SANDBOX
  buffer.SetBackingStoreRefForSerialization(external_pointer_entry);
#else
  buffer.set_backing_store(serializer_->isolate(), backing_store);
#endif
  buffer.set_extension(extension);
}

void Serializer::ObjectSerializer::SerializeExternalString() {
  // For external strings with known resources, we replace the resource field
  // with the encoded external reference, which we restore upon deserialize.
  // For the rest we serialize them to look like ordinary sequential strings.
  ExternalString string = ExternalString::cast(object_);
  Address resource = string.resource_as_address();
  ExternalReferenceEncoder::Value reference;
  if (serializer_->external_reference_encoder_.TryEncode(resource).To(
          &reference)) {
    DCHECK(reference.is_from_api());
#ifdef V8_HEAP_SANDBOX
    uint32_t external_pointer_entry = string.GetResourceRefForDeserialization();
#endif
    string.SetResourceRefForSerialization(reference.index());
    SerializeObject();
#ifdef V8_HEAP_SANDBOX
    string.SetResourceRefForSerialization(external_pointer_entry);
#else
    string.set_address_as_resource(serializer_->isolate(), resource);
#endif
  } else {
    SerializeExternalStringAsSequentialString();
  }
}

void Serializer::ObjectSerializer::SerializeExternalStringAsSequentialString() {
  // Instead of serializing this as an external string, we serialize
  // an imaginary sequential string with the same content.
  ReadOnlyRoots roots(serializer_->isolate());
  DCHECK(object_.IsExternalString());
  ExternalString string = ExternalString::cast(object_);
  int length = string.length();
  Map map;
  int content_size;
  int allocation_size;
  const byte* resource;
  // Find the map and size for the imaginary sequential string.
  bool internalized = object_.IsInternalizedString();
  if (object_.IsExternalOneByteString()) {
    map = internalized ? roots.one_byte_internalized_string_map()
                       : roots.one_byte_string_map();
    allocation_size = SeqOneByteString::SizeFor(length);
    content_size = length * kCharSize;
    resource = reinterpret_cast<const byte*>(
        ExternalOneByteString::cast(string).resource()->data());
  } else {
    map = internalized ? roots.internalized_string_map() : roots.string_map();
    allocation_size = SeqTwoByteString::SizeFor(length);
    content_size = length * kShortSize;
    resource = reinterpret_cast<const byte*>(
        ExternalTwoByteString::cast(string).resource()->data());
  }

  SnapshotSpace space = (allocation_size > kMaxRegularHeapObjectSize)
                            ? SnapshotSpace::kLargeObject
                            : SnapshotSpace::kOld;
  SerializePrologue(space, allocation_size, map);

  // Output the rest of the imaginary string.
  int bytes_to_output = allocation_size - HeapObject::kHeaderSize;
  DCHECK(IsAligned(bytes_to_output, kTaggedSize));

  // Output raw data header. Do not bother with common raw length cases here.
  sink_->Put(kVariableRawData, "RawDataForString");
  sink_->PutInt(bytes_to_output, "length");

  // Serialize string header (except for map).
  byte* string_start = reinterpret_cast<byte*>(string.address());
  for (int i = HeapObject::kHeaderSize; i < SeqString::kHeaderSize; i++) {
    sink_->Put(string_start[i], "StringHeader");
  }

  // Serialize string content.
  sink_->PutRaw(resource, content_size, "StringContent");

  // Since the allocation size is rounded up to object alignment, there
  // maybe left-over bytes that need to be padded.
  int padding_size = allocation_size - SeqString::kHeaderSize - content_size;
  DCHECK(0 <= padding_size && padding_size < kObjectAlignment);
  for (int i = 0; i < padding_size; i++)
    sink_->Put(static_cast<byte>(0), "StringPadding");
}

// Clear and later restore the next link in the weak cell or allocation site.
// TODO(all): replace this with proper iteration of weak slots in serializer.
class UnlinkWeakNextScope {
 public:
  explicit UnlinkWeakNextScope(Heap* heap, HeapObject object) {
    if (object.IsAllocationSite() &&
        AllocationSite::cast(object).HasWeakNext()) {
      object_ = object;
      next_ = AllocationSite::cast(object).weak_next();
      AllocationSite::cast(object).set_weak_next(
          ReadOnlyRoots(heap).undefined_value());
    }
  }

  ~UnlinkWeakNextScope() {
    if (!object_.is_null()) {
      AllocationSite::cast(object_).set_weak_next(next_,
                                                  UPDATE_WEAK_WRITE_BARRIER);
    }
  }

 private:
  HeapObject object_;
  Object next_;
  DISALLOW_HEAP_ALLOCATION(no_gc_)
};

void Serializer::ObjectSerializer::Serialize() {
  RecursionScope recursion(serializer_);

  // Defer objects as "pending" if they cannot be serialized now, or if we
  // exceed a certain recursion depth. Some objects cannot be deferred
  if ((recursion.ExceedsMaximum() && CanBeDeferred(object_)) ||
      serializer_->MustBeDeferred(object_)) {
    DCHECK(CanBeDeferred(object_));
    if (FLAG_trace_serializer) {
      PrintF(" Deferring heap object: ");
      object_.ShortPrint();
      PrintF("\n");
    }
    // Deferred objects are considered "pending".
    PendingObjectReference pending_obj =
        serializer_->RegisterObjectIsPending(object_);
    serializer_->PutPendingForwardReferenceTo(pending_obj);
    serializer_->QueueDeferredObject(object_);
    return;
  }

  if (FLAG_trace_serializer) {
    PrintF(" Encoding heap object: ");
    object_.ShortPrint();
    PrintF("\n");
  }

  if (object_.IsExternalString()) {
    SerializeExternalString();
    return;
  } else if (!ReadOnlyHeap::Contains(object_)) {
    // Only clear padding for strings outside the read-only heap. Read-only heap
    // should have been cleared elsewhere.
    if (object_.IsSeqOneByteString()) {
      // Clear padding bytes at the end. Done here to avoid having to do this
      // at allocation sites in generated code.
      SeqOneByteString::cast(object_).clear_padding();
    } else if (object_.IsSeqTwoByteString()) {
      SeqTwoByteString::cast(object_).clear_padding();
    }
  }
  if (object_.IsJSTypedArray()) {
    SerializeJSTypedArray();
    return;
  }
  if (object_.IsJSArrayBuffer()) {
    SerializeJSArrayBuffer();
    return;
  }

  // We don't expect fillers.
  DCHECK(!object_.IsFreeSpaceOrFiller());

  if (object_.IsScript()) {
    // Clear cached line ends.
    Oddball undefined = ReadOnlyRoots(serializer_->isolate()).undefined_value();
    Script::cast(object_).set_line_ends(undefined);
  }

  SerializeObject();
}

namespace {
SnapshotSpace GetSnapshotSpace(HeapObject object) {
  if (V8_ENABLE_THIRD_PARTY_HEAP_BOOL) {
    if (third_party_heap::Heap::InCodeSpace(object.address())) {
      return SnapshotSpace::kCode;
    } else if (ReadOnlyHeap::Contains(object)) {
      return SnapshotSpace::kReadOnlyHeap;
    } else if (object.Size() > kMaxRegularHeapObjectSize) {
      return SnapshotSpace::kLargeObject;
    } else if (object.IsMap()) {
      return SnapshotSpace::kMap;
    } else {
      return SnapshotSpace::kOld;  // avoid new/young distinction in TPH
    }
  } else if (ReadOnlyHeap::Contains(object)) {
    return SnapshotSpace::kReadOnlyHeap;
  } else {
    AllocationSpace heap_space =
        MemoryChunk::FromHeapObject(object)->owner_identity();
    // Large code objects are not supported and cannot be expressed by
    // SnapshotSpace.
    DCHECK_NE(heap_space, CODE_LO_SPACE);
    switch (heap_space) {
      // Young generation objects are tenured, as objects that have survived
      // until snapshot building probably deserve to be considered 'old'.
      case NEW_SPACE:
        return SnapshotSpace::kOld;
      case NEW_LO_SPACE:
        return SnapshotSpace::kLargeObject;

      default:
        return static_cast<SnapshotSpace>(heap_space);
    }
  }
}
}  // namespace

void Serializer::ObjectSerializer::SerializeObject() {
  int size = object_.Size();
  Map map = object_.map();
  SnapshotSpace space = GetSnapshotSpace(object_);
  SerializePrologue(space, size, map);

  // Serialize the rest of the object.
  CHECK_EQ(0, bytes_processed_so_far_);
  bytes_processed_so_far_ = kTaggedSize;

  SerializeContent(map, size);
}

void Serializer::ObjectSerializer::SerializeDeferred() {
  SerializerReference back_reference =
      serializer_->reference_map()->LookupReference(
          reinterpret_cast<void*>(object_.ptr()));

  if (back_reference.is_valid()) {
    if (FLAG_trace_serializer) {
      PrintF(" Deferred heap object ");
      object_.ShortPrint();
      PrintF(" was already serialized\n");
    }
    return;
  }

  if (FLAG_trace_serializer) {
    PrintF(" Encoding deferred heap object\n");
  }
  Serialize();
}

void Serializer::ObjectSerializer::SerializeContent(Map map, int size) {
  UnlinkWeakNextScope unlink_weak_next(serializer_->isolate()->heap(), object_);
  if (object_.IsCode()) {
    // For code objects, output raw bytes first.
    OutputCode(size);
    // Then iterate references via reloc info.
    object_.IterateBody(map, size, this);
  } else {
    // For other objects, iterate references first.
    object_.IterateBody(map, size, this);
    // Then output data payload, if any.
    OutputRawData(object_.address() + size);
  }
}

void Serializer::ObjectSerializer::VisitPointers(HeapObject host,
                                                 ObjectSlot start,
                                                 ObjectSlot end) {
  VisitPointers(host, MaybeObjectSlot(start), MaybeObjectSlot(end));
}

void Serializer::ObjectSerializer::VisitPointers(HeapObject host,
                                                 MaybeObjectSlot start,
                                                 MaybeObjectSlot end) {
  DisallowGarbageCollection no_gc;

  MaybeObjectSlot current = start;
  while (current < end) {
    while (current < end && (*current)->IsSmi()) {
      ++current;
    }
    if (current < end) {
      OutputRawData(current.address());
    }
    // TODO(ishell): Revisit this change once we stick to 32-bit compressed
    // tagged values.
    while (current < end && (*current)->IsCleared()) {
      sink_->Put(kClearedWeakReference, "ClearedWeakReference");
      bytes_processed_so_far_ += kTaggedSize;
      ++current;
    }
    HeapObject current_contents;
    HeapObjectReferenceType reference_type;
    while (current < end &&
           (*current)->GetHeapObject(&current_contents, &reference_type)) {
      // Write a weak prefix if we need it. This has to be done before the
      // potential pending object serialization.
      if (reference_type == HeapObjectReferenceType::WEAK) {
        sink_->Put(kWeakPrefix, "WeakReference");
      }

      if (serializer_->SerializePendingObject(current_contents)) {
        bytes_processed_so_far_ += kTaggedSize;
        ++current;
        continue;
      }

      RootIndex root_index;
      // Compute repeat count and write repeat prefix if applicable.
      // Repeats are not subject to the write barrier so we can only use
      // immortal immovable root members.
      MaybeObjectSlot repeat_end = current + 1;
      if (repeat_end < end &&
          serializer_->root_index_map()->Lookup(current_contents,
                                                &root_index) &&
          RootsTable::IsImmortalImmovable(root_index) &&
          *current == *repeat_end) {
        DCHECK_EQ(reference_type, HeapObjectReferenceType::STRONG);
        if (!V8_ENABLE_THIRD_PARTY_HEAP_BOOL)  
          DCHECK(!Heap::InYoungGeneration(current_contents));
        while (repeat_end < end && *repeat_end == *current) {
          repeat_end++;
        }
        int repeat_count = static_cast<int>(repeat_end - current);
        current = repeat_end;
        bytes_processed_so_far_ += repeat_count * kTaggedSize;
        serializer_->PutRepeat(repeat_count);
      } else {
        bytes_processed_so_far_ += kTaggedSize;
        ++current;
      }
      // Now write the object itself.
      serializer_->SerializeObject(current_contents);
    }
  }
}

void Serializer::ObjectSerializer::VisitEmbeddedPointer(Code host,
                                                        RelocInfo* rinfo) {
  Object object = rinfo->target_object();
  serializer_->SerializeObject(HeapObject::cast(object));
  bytes_processed_so_far_ += rinfo->target_address_size();
}

void Serializer::ObjectSerializer::OutputExternalReference(Address target,
                                                           int target_size,
                                                           bool sandboxify) {
  DCHECK_LE(target_size, sizeof(target));  // Must fit in Address.
  ExternalReferenceEncoder::Value encoded_reference;
  bool encoded_successfully;

  if (serializer_->allow_unknown_external_references_for_testing()) {
    encoded_successfully =
        serializer_->TryEncodeExternalReference(target).To(&encoded_reference);
  } else {
    encoded_reference = serializer_->EncodeExternalReference(target);
    encoded_successfully = true;
  }

  if (!encoded_successfully) {
    // In this case the serialized snapshot will not be used in a different
    // Isolate and thus the target address will not change between
    // serialization and deserialization. We can serialize seen external
    // references verbatim.
    CHECK(serializer_->allow_unknown_external_references_for_testing());
    CHECK(IsAligned(target_size, kObjectAlignment));
    CHECK_LE(target_size, kFixedRawDataCount * kTaggedSize);
    int size_in_tagged = target_size >> kTaggedSizeLog2;
    sink_->Put(FixedRawDataWithSize::Encode(size_in_tagged), "FixedRawData");
    sink_->PutRaw(reinterpret_cast<byte*>(&target), target_size, "Bytes");
  } else if (encoded_reference.is_from_api()) {
    if (V8_HEAP_SANDBOX_BOOL && sandboxify) {
      sink_->Put(kSandboxedApiReference, "SandboxedApiRef");
    } else {
      sink_->Put(kApiReference, "ApiRef");
    }
    sink_->PutInt(encoded_reference.index(), "reference index");
  } else {
    if (V8_HEAP_SANDBOX_BOOL && sandboxify) {
      sink_->Put(kSandboxedExternalReference, "SandboxedExternalRef");
    } else {
      sink_->Put(kExternalReference, "ExternalRef");
    }
    sink_->PutInt(encoded_reference.index(), "reference index");
  }
  bytes_processed_so_far_ += target_size;
}

void Serializer::ObjectSerializer::VisitExternalReference(Foreign host,
                                                          Address* p) {
  // "Sandboxify" external reference.
  OutputExternalReference(host.foreign_address(), kExternalPointerSize, true);
}

void Serializer::ObjectSerializer::VisitExternalReference(Code host,
                                                          RelocInfo* rinfo) {
  Address target = rinfo->target_external_reference();
  DCHECK_NE(target, kNullAddress);  // Code does not reference null.
  DCHECK_IMPLIES(serializer_->EncodeExternalReference(target).is_from_api(),
                 !rinfo->IsCodedSpecially());
  // Don't "sandboxify" external references embedded in the code.
  OutputExternalReference(target, rinfo->target_address_size(), false);
}

void Serializer::ObjectSerializer::VisitInternalReference(Code host,
                                                          RelocInfo* rinfo) {
  Address entry = Code::cast(object_).entry();
  DCHECK_GE(rinfo->target_internal_reference(), entry);
  uintptr_t target_offset = rinfo->target_internal_reference() - entry;
  DCHECK_LE(target_offset, Code::cast(object_).raw_instruction_size());
  sink_->Put(kInternalReference, "InternalRef");
  sink_->PutInt(target_offset, "internal ref value");
}

void Serializer::ObjectSerializer::VisitRuntimeEntry(Code host,
                                                     RelocInfo* rinfo) {
  // We no longer serialize code that contains runtime entries.
  UNREACHABLE();
}

void Serializer::ObjectSerializer::VisitOffHeapTarget(Code host,
                                                      RelocInfo* rinfo) {
  STATIC_ASSERT(EmbeddedData::kTableSize == Builtins::builtin_count);

  Address addr = rinfo->target_off_heap_target();
  CHECK_NE(kNullAddress, addr);

  Code target = InstructionStream::TryLookupCode(serializer_->isolate(), addr);
  CHECK(Builtins::IsIsolateIndependentBuiltin(target));

  sink_->Put(kOffHeapTarget, "OffHeapTarget");
  sink_->PutInt(target.builtin_index(), "builtin index");
  bytes_processed_so_far_ += rinfo->target_address_size();
}

void Serializer::ObjectSerializer::VisitCodeTarget(Code host,
                                                   RelocInfo* rinfo) {
#ifdef V8_TARGET_ARCH_ARM
  DCHECK(!RelocInfo::IsRelativeCodeTarget(rinfo->rmode()));
#endif
  Code object = Code::GetCodeFromTargetAddress(rinfo->target_address());
  serializer_->SerializeObject(object);
  bytes_processed_so_far_ += rinfo->target_address_size();
}

namespace {

// Similar to OutputRawData, but substitutes the given field with the given
// value instead of reading it from the object.
void OutputRawWithCustomField(SnapshotByteSink* sink, Address object_start,
                              int written_so_far, int bytes_to_write,
                              int field_offset, int field_size,
                              const byte* field_value) {
  int offset = field_offset - written_so_far;
  if (0 <= offset && offset < bytes_to_write) {
    DCHECK_GE(bytes_to_write, offset + field_size);
    sink->PutRaw(reinterpret_cast<byte*>(object_start + written_so_far), offset,
                 "Bytes");
    sink->PutRaw(field_value, field_size, "Bytes");
    written_so_far += offset + field_size;
    bytes_to_write -= offset + field_size;
    sink->PutRaw(reinterpret_cast<byte*>(object_start + written_so_far),
                 bytes_to_write, "Bytes");
  } else {
    sink->PutRaw(reinterpret_cast<byte*>(object_start + written_so_far),
                 bytes_to_write, "Bytes");
  }
}
}  // anonymous namespace

void Serializer::ObjectSerializer::OutputRawData(Address up_to) {
  Address object_start = object_.address();
  int base = bytes_processed_so_far_;
  int up_to_offset = static_cast<int>(up_to - object_start);
  int to_skip = up_to_offset - bytes_processed_so_far_;
  int bytes_to_output = to_skip;
  bytes_processed_so_far_ += to_skip;
  DCHECK_GE(to_skip, 0);
  if (bytes_to_output != 0) {
    DCHECK(to_skip == bytes_to_output);
    if (IsAligned(bytes_to_output, kObjectAlignment) &&
        bytes_to_output <= kFixedRawDataCount * kTaggedSize) {
      int size_in_tagged = bytes_to_output >> kTaggedSizeLog2;
      sink_->Put(FixedRawDataWithSize::Encode(size_in_tagged), "FixedRawData");
    } else {
      sink_->Put(kVariableRawData, "VariableRawData");
      sink_->PutInt(bytes_to_output, "length");
    }
#ifdef MEMORY_SANITIZER
    // Check that we do not serialize uninitialized memory.
    __msan_check_mem_is_initialized(
        reinterpret_cast<void*>(object_start + base), bytes_to_output);
#endif  // MEMORY_SANITIZER
    if (object_.IsBytecodeArray()) {
      // The bytecode age field can be changed by GC concurrently.
      byte field_value = BytecodeArray::kNoAgeBytecodeAge;
      OutputRawWithCustomField(sink_, object_start, base, bytes_to_output,
                               BytecodeArray::kBytecodeAgeOffset,
                               sizeof(field_value), &field_value);
    } else if (object_.IsDescriptorArray()) {
      // The number of marked descriptors field can be changed by GC
      // concurrently.
      byte field_value[2];
      field_value[0] = 0;
      field_value[1] = 0;
      OutputRawWithCustomField(
          sink_, object_start, base, bytes_to_output,
          DescriptorArray::kRawNumberOfMarkedDescriptorsOffset,
          sizeof(field_value), field_value);
    } else {
      sink_->PutRaw(reinterpret_cast<byte*>(object_start + base),
                    bytes_to_output, "Bytes");
    }
  }
}

void Serializer::ObjectSerializer::OutputCode(int size) {
  DCHECK_EQ(kTaggedSize, bytes_processed_so_far_);
  Code on_heap_code = Code::cast(object_);
  // To make snapshots reproducible, we make a copy of the code object
  // and wipe all pointers in the copy, which we then serialize.
  Code off_heap_code = serializer_->CopyCode(on_heap_code);
  int mode_mask = RelocInfo::ModeMask(RelocInfo::CODE_TARGET) |
                  RelocInfo::ModeMask(RelocInfo::FULL_EMBEDDED_OBJECT) |
                  RelocInfo::ModeMask(RelocInfo::COMPRESSED_EMBEDDED_OBJECT) |
                  RelocInfo::ModeMask(RelocInfo::EXTERNAL_REFERENCE) |
                  RelocInfo::ModeMask(RelocInfo::INTERNAL_REFERENCE) |
                  RelocInfo::ModeMask(RelocInfo::INTERNAL_REFERENCE_ENCODED) |
                  RelocInfo::ModeMask(RelocInfo::OFF_HEAP_TARGET) |
                  RelocInfo::ModeMask(RelocInfo::RUNTIME_ENTRY);
  // With enabled pointer compression normal accessors no longer work for
  // off-heap objects, so we have to get the relocation info data via the
  // on-heap code object.
  ByteArray relocation_info = on_heap_code.unchecked_relocation_info();
  for (RelocIterator it(off_heap_code, relocation_info, mode_mask); !it.done();
       it.next()) {
    RelocInfo* rinfo = it.rinfo();
    rinfo->WipeOut();
  }
  // We need to wipe out the header fields *after* wiping out the
  // relocations, because some of these fields are needed for the latter.
  off_heap_code.WipeOutHeader();

  Address start = off_heap_code.address() + Code::kDataStart;
  int bytes_to_output = size - Code::kDataStart;
  DCHECK(IsAligned(bytes_to_output, kTaggedSize));

  sink_->Put(kVariableRawCode, "VariableRawCode");
  sink_->PutInt(bytes_to_output, "length");

#ifdef MEMORY_SANITIZER
  // Check that we do not serialize uninitialized memory.
  __msan_check_mem_is_initialized(reinterpret_cast<void*>(start),
                                  bytes_to_output);
#endif  // MEMORY_SANITIZER
  sink_->PutRaw(reinterpret_cast<byte*>(start), bytes_to_output, "Code");
}

}  // namespace internal
}  // namespace v8
