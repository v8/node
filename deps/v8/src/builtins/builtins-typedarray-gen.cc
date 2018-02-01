// Copyright 2017 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/builtins/builtins-constructor-gen.h"
#include "src/builtins/builtins-utils-gen.h"
#include "src/builtins/builtins.h"
#include "src/code-stub-assembler.h"
#include "src/handles-inl.h"

namespace v8 {
namespace internal {

using compiler::Node;
template <class T>
using TNode = compiler::TNode<T>;

// This is needed for gc_mole which will compile this file without the full set
// of GN defined macros.
#ifndef V8_TYPED_ARRAY_MAX_SIZE_IN_HEAP
#define V8_TYPED_ARRAY_MAX_SIZE_IN_HEAP 64
#endif

// -----------------------------------------------------------------------------
// ES6 section 22.2 TypedArray Objects

class TypedArrayBuiltinsAssembler : public CodeStubAssembler {
 public:
  explicit TypedArrayBuiltinsAssembler(compiler::CodeAssemblerState* state)
      : CodeStubAssembler(state) {}

 protected:
  void GenerateTypedArrayPrototypeGetter(Node* context, Node* receiver,
                                         const char* method_name,
                                         int object_offset);
  void GenerateTypedArrayPrototypeIterationMethod(Node* context, Node* receiver,
                                                  const char* method_name,
                                                  IterationKind iteration_kind);

  void ConstructByLength(TNode<Context> context, TNode<JSTypedArray> holder,
                         TNode<Object> length, TNode<Smi> element_size);
  void ConstructByArrayBuffer(TNode<Context> context,
                              TNode<JSTypedArray> holder,
                              TNode<JSArrayBuffer> buffer,
                              TNode<Object> byte_offset, TNode<Object> length,
                              TNode<Smi> element_size);
  void ConstructByTypedArray(TNode<Context> context, TNode<JSTypedArray> holder,
                             TNode<JSTypedArray> typed_array,
                             TNode<Smi> element_size);
  void ConstructByArrayLike(TNode<Context> context, TNode<JSTypedArray> holder,
                            TNode<HeapObject> array_like,
                            TNode<Object> initial_length,
                            TNode<Smi> element_size);

  void SetupTypedArray(TNode<JSTypedArray> holder, TNode<Smi> length,
                       TNode<Number> byte_offset, TNode<Number> byte_length);
  void AttachBuffer(TNode<JSTypedArray> holder, TNode<JSArrayBuffer> buffer,
                    TNode<Map> map, TNode<Smi> length,
                    TNode<Number> byte_offset);

  TNode<Map> LoadMapForType(TNode<JSTypedArray> array);
  TNode<UintPtrT> CalculateExternalPointer(TNode<UintPtrT> backing_store,
                                           TNode<Number> byte_offset);
  Node* LoadDataPtr(Node* typed_array);
  TNode<BoolT> ByteLengthIsValid(TNode<Number> byte_length);

  // Returns true if kind is either UINT8_ELEMENTS or UINT8_CLAMPED_ELEMENTS.
  TNode<Word32T> IsUint8ElementsKind(TNode<Word32T> kind);

  // Loads the element kind of TypedArray instance.
  TNode<Word32T> LoadElementsKind(TNode<Object> typed_array);

  // Returns the byte size of an element for a TypedArray elements kind.
  TNode<IntPtrT> GetTypedArrayElementSize(TNode<Word32T> elements_kind);

  TNode<Object> GetDefaultConstructor(TNode<Context> context,
                                      TNode<JSTypedArray> exemplar);

  TNode<Object> TypedArraySpeciesConstructor(TNode<Context> context,
                                             TNode<JSTypedArray> exemplar);

  TNode<JSTypedArray> SpeciesCreateByArrayBuffer(TNode<Context> context,
                                                 TNode<JSTypedArray> exemplar,
                                                 TNode<JSArrayBuffer> buffer,
                                                 TNode<Number> byte_offset,
                                                 TNode<Smi> len,
                                                 const char* method_name);

  TNode<JSTypedArray> SpeciesCreateByLength(TNode<Context> context,
                                            TNode<JSTypedArray> exemplar,
                                            TNode<Smi> len,
                                            const char* method_name);

  TNode<JSArrayBuffer> GetBuffer(TNode<Context> context,
                                 TNode<JSTypedArray> array);

  TNode<JSTypedArray> ValidateTypedArray(TNode<Context> context,
                                         TNode<Object> obj,
                                         const char* method_name);

  // Fast path for setting a TypedArray (source) onto another TypedArray
  // (target) at an element offset.
  void SetTypedArraySource(TNode<Context> context, TNode<JSTypedArray> source,
                           TNode<JSTypedArray> target, TNode<IntPtrT> offset,
                           Label* call_runtime, Label* if_source_too_large);

  void SetJSArraySource(TNode<Context> context, TNode<JSArray> source,
                        TNode<JSTypedArray> target, TNode<IntPtrT> offset,
                        Label* call_runtime, Label* if_source_too_large);

  void CallCMemmove(TNode<IntPtrT> dest_ptr, TNode<IntPtrT> src_ptr,
                    TNode<IntPtrT> byte_length);

  void CallCCopyFastNumberJSArrayElementsToTypedArray(
      TNode<Context> context, TNode<JSArray> source, TNode<JSTypedArray> dest,
      TNode<IntPtrT> source_length, TNode<IntPtrT> offset);

  void CallCCopyTypedArrayElementsToTypedArray(TNode<JSTypedArray> source,
                                               TNode<JSTypedArray> dest,
                                               TNode<IntPtrT> source_length,
                                               TNode<IntPtrT> offset);
};

TNode<Map> TypedArrayBuiltinsAssembler::LoadMapForType(
    TNode<JSTypedArray> array) {
  Label unreachable(this), done(this);
  Label uint8_elements(this), uint8_clamped_elements(this), int8_elements(this),
      uint16_elements(this), int16_elements(this), uint32_elements(this),
      int32_elements(this), float32_elements(this), float64_elements(this);
  Label* elements_kind_labels[] = {
      &uint8_elements,  &uint8_clamped_elements, &int8_elements,
      &uint16_elements, &int16_elements,         &uint32_elements,
      &int32_elements,  &float32_elements,       &float64_elements};
  int32_t elements_kinds[] = {
      UINT8_ELEMENTS,  UINT8_CLAMPED_ELEMENTS, INT8_ELEMENTS,
      UINT16_ELEMENTS, INT16_ELEMENTS,         UINT32_ELEMENTS,
      INT32_ELEMENTS,  FLOAT32_ELEMENTS,       FLOAT64_ELEMENTS};
  const size_t kTypedElementsKindCount = LAST_FIXED_TYPED_ARRAY_ELEMENTS_KIND -
                                         FIRST_FIXED_TYPED_ARRAY_ELEMENTS_KIND +
                                         1;
  DCHECK_EQ(kTypedElementsKindCount, arraysize(elements_kinds));
  DCHECK_EQ(kTypedElementsKindCount, arraysize(elements_kind_labels));

  TVARIABLE(Map, var_typed_map);

  TNode<Map> array_map = LoadMap(array);
  TNode<Int32T> elements_kind = LoadMapElementsKind(array_map);
  Switch(elements_kind, &unreachable, elements_kinds, elements_kind_labels,
         kTypedElementsKindCount);

  for (int i = 0; i < static_cast<int>(kTypedElementsKindCount); i++) {
    BIND(elements_kind_labels[i]);
    {
      ElementsKind kind = static_cast<ElementsKind>(elements_kinds[i]);
      ExternalArrayType type =
          isolate()->factory()->GetArrayTypeFromElementsKind(kind);
      Handle<Map> map(isolate()->heap()->MapForFixedTypedArray(type));
      var_typed_map = HeapConstant(map);
      Goto(&done);
    }
  }

  BIND(&unreachable);
  { Unreachable(); }
  BIND(&done);
  return var_typed_map;
}

// The byte_offset can be higher than Smi range, in which case to perform the
// pointer arithmetic necessary to calculate external_pointer, converting
// byte_offset to an intptr is more difficult. The max byte_offset is 8 * MaxSmi
// on the particular platform. 32 bit platforms are self-limiting, because we
// can't allocate an array bigger than our 32-bit arithmetic range anyway. 64
// bit platforms could theoretically have an offset up to 2^35 - 1, so we may
// need to convert the float heap number to an intptr.
TNode<UintPtrT> TypedArrayBuiltinsAssembler::CalculateExternalPointer(
    TNode<UintPtrT> backing_store, TNode<Number> byte_offset) {
  return Unsigned(
      IntPtrAdd(backing_store, ChangeNonnegativeNumberToUintPtr(byte_offset)));
}

// Setup the TypedArray which is under construction.
//  - Set the length.
//  - Set the byte_offset.
//  - Set the byte_length.
//  - Set EmbedderFields to 0.
void TypedArrayBuiltinsAssembler::SetupTypedArray(TNode<JSTypedArray> holder,
                                                  TNode<Smi> length,
                                                  TNode<Number> byte_offset,
                                                  TNode<Number> byte_length) {
  StoreObjectField(holder, JSTypedArray::kLengthOffset, length);
  StoreObjectField(holder, JSArrayBufferView::kByteOffsetOffset, byte_offset);
  StoreObjectField(holder, JSArrayBufferView::kByteLengthOffset, byte_length);
  for (int offset = JSTypedArray::kSize;
       offset < JSTypedArray::kSizeWithEmbedderFields; offset += kPointerSize) {
    StoreObjectField(holder, offset, SmiConstant(0));
  }
}

// Attach an off-heap buffer to a TypedArray.
void TypedArrayBuiltinsAssembler::AttachBuffer(TNode<JSTypedArray> holder,
                                               TNode<JSArrayBuffer> buffer,
                                               TNode<Map> map,
                                               TNode<Smi> length,
                                               TNode<Number> byte_offset) {
  StoreObjectField(holder, JSArrayBufferView::kBufferOffset, buffer);

  Node* elements = Allocate(FixedTypedArrayBase::kHeaderSize);
  StoreMapNoWriteBarrier(elements, map);
  StoreObjectFieldNoWriteBarrier(elements, FixedArray::kLengthOffset, length);
  StoreObjectFieldNoWriteBarrier(
      elements, FixedTypedArrayBase::kBasePointerOffset, SmiConstant(0));

  TNode<UintPtrT> backing_store =
      LoadObjectField<UintPtrT>(buffer, JSArrayBuffer::kBackingStoreOffset);

  TNode<UintPtrT> external_pointer =
      CalculateExternalPointer(backing_store, byte_offset);
  StoreObjectFieldNoWriteBarrier(
      elements, FixedTypedArrayBase::kExternalPointerOffset, external_pointer,
      MachineType::PointerRepresentation());

  StoreObjectField(holder, JSObject::kElementsOffset, elements);
}

TF_BUILTIN(TypedArrayInitializeWithBuffer, TypedArrayBuiltinsAssembler) {
  TNode<JSTypedArray> holder = CAST(Parameter(Descriptor::kHolder));
  TNode<Smi> length = CAST(Parameter(Descriptor::kLength));
  TNode<JSArrayBuffer> buffer = CAST(Parameter(Descriptor::kBuffer));
  TNode<Smi> element_size = CAST(Parameter(Descriptor::kElementSize));
  TNode<Number> byte_offset = CAST(Parameter(Descriptor::kByteOffset));

  TNode<Map> fixed_typed_map = LoadMapForType(holder);

  // SmiMul returns a heap number in case of Smi overflow.
  TNode<Number> byte_length = SmiMul(length, element_size);

  SetupTypedArray(holder, length, byte_offset, byte_length);
  AttachBuffer(holder, buffer, fixed_typed_map, length, byte_offset);
  Return(UndefinedConstant());
}

TF_BUILTIN(TypedArrayInitialize, TypedArrayBuiltinsAssembler) {
  TNode<JSTypedArray> holder = CAST(Parameter(Descriptor::kHolder));
  TNode<Smi> length = CAST(Parameter(Descriptor::kLength));
  TNode<Smi> element_size = CAST(Parameter(Descriptor::kElementSize));
  Node* initialize = Parameter(Descriptor::kInitialize);
  TNode<Context> context = CAST(Parameter(Descriptor::kContext));

  CSA_ASSERT(this, TaggedIsPositiveSmi(length));
  CSA_ASSERT(this, TaggedIsPositiveSmi(element_size));
  CSA_ASSERT(this, IsBoolean(initialize));

  TNode<Smi> byte_offset = SmiConstant(0);

  static const int32_t fta_base_data_offset =
      FixedTypedArrayBase::kDataOffset - kHeapObjectTag;

  Label setup_holder(this), allocate_on_heap(this), aligned(this),
      allocate_elements(this), allocate_off_heap(this),
      allocate_off_heap_no_init(this), attach_buffer(this), done(this);
  VARIABLE(var_total_size, MachineType::PointerRepresentation());

  // SmiMul returns a heap number in case of Smi overflow.
  TNode<Number> byte_length = SmiMul(length, element_size);

  SetupTypedArray(holder, length, byte_offset, byte_length);

  TNode<Map> fixed_typed_map = LoadMapForType(holder);
  GotoIf(TaggedIsNotSmi(byte_length), &allocate_off_heap);
  GotoIf(
      SmiGreaterThan(byte_length, SmiConstant(V8_TYPED_ARRAY_MAX_SIZE_IN_HEAP)),
      &allocate_off_heap);
  TNode<IntPtrT> word_byte_length = SmiToWord(CAST(byte_length));
  Goto(&allocate_on_heap);

  BIND(&allocate_on_heap);
  {
    CSA_ASSERT(this, TaggedIsPositiveSmi(byte_length));
    // Allocate a new ArrayBuffer and initialize it with empty properties and
    // elements.
    Node* native_context = LoadNativeContext(context);
    Node* map =
        LoadContextElement(native_context, Context::ARRAY_BUFFER_MAP_INDEX);
    Node* empty_fixed_array = LoadRoot(Heap::kEmptyFixedArrayRootIndex);

    Node* buffer = Allocate(JSArrayBuffer::kSizeWithEmbedderFields);
    StoreMapNoWriteBarrier(buffer, map);
    StoreObjectFieldNoWriteBarrier(buffer, JSArray::kPropertiesOrHashOffset,
                                   empty_fixed_array);
    StoreObjectFieldNoWriteBarrier(buffer, JSArray::kElementsOffset,
                                   empty_fixed_array);
    // Setup the ArrayBuffer.
    //  - Set BitField to 0.
    //  - Set IsExternal and IsNeuterable bits of BitFieldSlot.
    //  - Set the byte_length field to byte_length.
    //  - Set backing_store to null/Smi(0).
    //  - Set all embedder fields to Smi(0).
    StoreObjectFieldNoWriteBarrier(buffer, JSArrayBuffer::kBitFieldSlot,
                                   SmiConstant(0));
    int32_t bitfield_value = (1 << JSArrayBuffer::IsExternal::kShift) |
                             (1 << JSArrayBuffer::IsNeuterable::kShift);
    StoreObjectFieldNoWriteBarrier(buffer, JSArrayBuffer::kBitFieldOffset,
                                   Int32Constant(bitfield_value),
                                   MachineRepresentation::kWord32);

    StoreObjectFieldNoWriteBarrier(buffer, JSArrayBuffer::kByteLengthOffset,
                                   byte_length);
    StoreObjectFieldNoWriteBarrier(buffer, JSArrayBuffer::kBackingStoreOffset,
                                   SmiConstant(0));
    for (int i = 0; i < v8::ArrayBuffer::kEmbedderFieldCount; i++) {
      int offset = JSArrayBuffer::kSize + i * kPointerSize;
      StoreObjectFieldNoWriteBarrier(buffer, offset, SmiConstant(0));
    }

    StoreObjectField(holder, JSArrayBufferView::kBufferOffset, buffer);

    // Check the alignment.
    GotoIf(SmiEqual(SmiMod(element_size, SmiConstant(kObjectAlignment)),
                    SmiConstant(0)),
           &aligned);

    // Fix alignment if needed.
    DCHECK_EQ(0, FixedTypedArrayBase::kHeaderSize & kObjectAlignmentMask);
    Node* aligned_header_size =
        IntPtrConstant(FixedTypedArrayBase::kHeaderSize + kObjectAlignmentMask);
    Node* size = IntPtrAdd(word_byte_length, aligned_header_size);
    var_total_size.Bind(WordAnd(size, IntPtrConstant(~kObjectAlignmentMask)));
    Goto(&allocate_elements);
  }

  BIND(&aligned);
  {
    Node* header_size = IntPtrConstant(FixedTypedArrayBase::kHeaderSize);
    var_total_size.Bind(IntPtrAdd(word_byte_length, header_size));
    Goto(&allocate_elements);
  }

  BIND(&allocate_elements);
  {
    // Allocate a FixedTypedArray and set the length, base pointer and external
    // pointer.
    CSA_ASSERT(this, IsRegularHeapObjectSize(var_total_size.value()));

    Node* elements;

    if (UnalignedLoadSupported(MachineRepresentation::kFloat64) &&
        UnalignedStoreSupported(MachineRepresentation::kFloat64)) {
      elements = AllocateInNewSpace(var_total_size.value());
    } else {
      elements = AllocateInNewSpace(var_total_size.value(), kDoubleAlignment);
    }

    StoreMapNoWriteBarrier(elements, fixed_typed_map);
    StoreObjectFieldNoWriteBarrier(elements, FixedArray::kLengthOffset, length);
    StoreObjectFieldNoWriteBarrier(
        elements, FixedTypedArrayBase::kBasePointerOffset, elements);
    StoreObjectFieldNoWriteBarrier(elements,
                                   FixedTypedArrayBase::kExternalPointerOffset,
                                   IntPtrConstant(fta_base_data_offset),
                                   MachineType::PointerRepresentation());

    StoreObjectField(holder, JSObject::kElementsOffset, elements);

    GotoIf(IsFalse(initialize), &done);
    // Initialize the backing store by filling it with 0s.
    Node* backing_store = IntPtrAdd(BitcastTaggedToWord(elements),
                                    IntPtrConstant(fta_base_data_offset));
    // Call out to memset to perform initialization.
    Node* memset =
        ExternalConstant(ExternalReference::libc_memset_function(isolate()));
    CallCFunction3(MachineType::AnyTagged(), MachineType::Pointer(),
                   MachineType::IntPtr(), MachineType::UintPtr(), memset,
                   backing_store, IntPtrConstant(0), word_byte_length);
    Goto(&done);
  }

  TVARIABLE(JSArrayBuffer, var_buffer);

  BIND(&allocate_off_heap);
  {
    GotoIf(IsFalse(initialize), &allocate_off_heap_no_init);

    Node* buffer_constructor = LoadContextElement(
        LoadNativeContext(context), Context::ARRAY_BUFFER_FUN_INDEX);
    var_buffer = CAST(ConstructJS(CodeFactory::Construct(isolate()), context,
                                  buffer_constructor, byte_length));
    Goto(&attach_buffer);
  }

  BIND(&allocate_off_heap_no_init);
  {
    Node* buffer_constructor_noinit = LoadContextElement(
        LoadNativeContext(context), Context::ARRAY_BUFFER_NOINIT_FUN_INDEX);
    var_buffer = CAST(CallJS(CodeFactory::Call(isolate()), context,
                             buffer_constructor_noinit, UndefinedConstant(),
                             byte_length));
    Goto(&attach_buffer);
  }

  BIND(&attach_buffer);
  {
    AttachBuffer(holder, var_buffer, fixed_typed_map, length, byte_offset);
    Goto(&done);
  }

  BIND(&done);
  Return(UndefinedConstant());
}

// ES6 #sec-typedarray-length
void TypedArrayBuiltinsAssembler::ConstructByLength(TNode<Context> context,
                                                    TNode<JSTypedArray> holder,
                                                    TNode<Object> length,
                                                    TNode<Smi> element_size) {
  CSA_ASSERT(this, TaggedIsPositiveSmi(element_size));

  Label invalid_length(this, Label::kDeferred), done(this);

  TNode<Number> converted_length =
      ToInteger_Inline(context, length, CodeStubAssembler::kTruncateMinusZero);

  // The maximum length of a TypedArray is MaxSmi().
  // Note: this is not per spec, but rather a constraint of our current
  // representation (which uses Smis).
  GotoIf(TaggedIsNotSmi(converted_length), &invalid_length);
  GotoIf(SmiLessThan(converted_length, SmiConstant(0)), &invalid_length);

  Node* initialize = TrueConstant();
  CallBuiltin(Builtins::kTypedArrayInitialize, context, holder,
              converted_length, element_size, initialize);
  Goto(&done);

  BIND(&invalid_length);
  {
    CallRuntime(Runtime::kThrowRangeError, context,
                SmiConstant(MessageTemplate::kInvalidTypedArrayLength),
                converted_length);
    Unreachable();
  }

  BIND(&done);
}

// ES6 #sec-typedarray-buffer-byteoffset-length
void TypedArrayBuiltinsAssembler::ConstructByArrayBuffer(
    TNode<Context> context, TNode<JSTypedArray> holder,
    TNode<JSArrayBuffer> buffer, TNode<Object> byte_offset,
    TNode<Object> length, TNode<Smi> element_size) {
  CSA_ASSERT(this, TaggedIsPositiveSmi(element_size));

  VARIABLE(new_byte_length, MachineRepresentation::kTagged, SmiConstant(0));
  VARIABLE(offset, MachineRepresentation::kTagged, SmiConstant(0));

  Label start_offset_error(this, Label::kDeferred),
      byte_length_error(this, Label::kDeferred),
      invalid_offset_error(this, Label::kDeferred);
  Label offset_is_smi(this), offset_not_smi(this, Label::kDeferred),
      check_length(this), call_init(this), invalid_length(this),
      length_undefined(this), length_defined(this), detached_error(this),
      done(this);

  GotoIf(IsUndefined(byte_offset), &check_length);

  offset.Bind(ToInteger_Inline(context, byte_offset,
                               CodeStubAssembler::kTruncateMinusZero));
  Branch(TaggedIsSmi(offset.value()), &offset_is_smi, &offset_not_smi);

  // Check that the offset is a multiple of the element size.
  BIND(&offset_is_smi);
  {
    GotoIf(SmiEqual(offset.value(), SmiConstant(0)), &check_length);
    GotoIf(SmiLessThan(offset.value(), SmiConstant(0)), &invalid_length);
    Node* remainder = SmiMod(offset.value(), element_size);
    Branch(SmiEqual(remainder, SmiConstant(0)), &check_length,
           &start_offset_error);
  }
  BIND(&offset_not_smi);
  {
    GotoIf(IsTrue(CallBuiltin(Builtins::kLessThan, context, offset.value(),
                              SmiConstant(0))),
           &invalid_length);
    Node* remainder =
        CallBuiltin(Builtins::kModulus, context, offset.value(), element_size);
    // Remainder can be a heap number.
    Branch(IsTrue(CallBuiltin(Builtins::kEqual, context, remainder,
                              SmiConstant(0))),
           &check_length, &start_offset_error);
  }

  BIND(&check_length);
  Branch(IsUndefined(length), &length_undefined, &length_defined);

  BIND(&length_undefined);
  {
    GotoIf(IsDetachedBuffer(buffer), &detached_error);
    Node* buffer_byte_length =
        LoadObjectField(buffer, JSArrayBuffer::kByteLengthOffset);

    Node* remainder = CallBuiltin(Builtins::kModulus, context,
                                  buffer_byte_length, element_size);
    // Remainder can be a heap number.
    GotoIf(IsFalse(CallBuiltin(Builtins::kEqual, context, remainder,
                               SmiConstant(0))),
           &byte_length_error);

    new_byte_length.Bind(CallBuiltin(Builtins::kSubtract, context,
                                     buffer_byte_length, offset.value()));

    Branch(IsTrue(CallBuiltin(Builtins::kLessThan, context,
                              new_byte_length.value(), SmiConstant(0))),
           &invalid_offset_error, &call_init);
  }

  BIND(&length_defined);
  {
    Node* new_length = ToSmiIndex(length, context, &invalid_length);
    GotoIf(IsDetachedBuffer(buffer), &detached_error);
    new_byte_length.Bind(SmiMul(new_length, element_size));
    // Reading the byte length must come after the ToIndex operation, which
    // could cause the buffer to become detached.
    Node* buffer_byte_length =
        LoadObjectField(buffer, JSArrayBuffer::kByteLengthOffset);

    Node* end = CallBuiltin(Builtins::kAdd, context, offset.value(),
                            new_byte_length.value());

    Branch(IsTrue(CallBuiltin(Builtins::kGreaterThan, context, end,
                              buffer_byte_length)),
           &invalid_length, &call_init);
  }

  BIND(&call_init);
  {
    Node* new_length = CallBuiltin(Builtins::kDivide, context,
                                   new_byte_length.value(), element_size);
    // Force the result into a Smi, or throw a range error if it doesn't fit.
    new_length = ToSmiIndex(new_length, context, &invalid_length);

    CallBuiltin(Builtins::kTypedArrayInitializeWithBuffer, context, holder,
                new_length, buffer, element_size, offset.value());
    Goto(&done);
  }

  BIND(&invalid_offset_error);
  {
    CallRuntime(Runtime::kThrowRangeError, context,
                SmiConstant(MessageTemplate::kInvalidOffset), byte_offset);
    Unreachable();
  }

  BIND(&start_offset_error);
  {
    Node* holder_map = LoadMap(holder);
    Node* problem_string = StringConstant("start offset");
    CallRuntime(Runtime::kThrowInvalidTypedArrayAlignment, context, holder_map,
                problem_string);

    Unreachable();
  }

  BIND(&byte_length_error);
  {
    Node* holder_map = LoadMap(holder);
    Node* problem_string = StringConstant("byte length");
    CallRuntime(Runtime::kThrowInvalidTypedArrayAlignment, context, holder_map,
                problem_string);

    Unreachable();
  }

  BIND(&invalid_length);
  {
    CallRuntime(Runtime::kThrowRangeError, context,
                SmiConstant(MessageTemplate::kInvalidTypedArrayLength), length);
    Unreachable();
  }

  BIND(&detached_error);
  { ThrowTypeError(context, MessageTemplate::kDetachedOperation, "Construct"); }

  BIND(&done);
}

void TypedArrayBuiltinsAssembler::ConstructByTypedArray(
    TNode<Context> context, TNode<JSTypedArray> holder,
    TNode<JSTypedArray> typed_array, TNode<Smi> element_size) {
  CSA_ASSERT(this, TaggedIsPositiveSmi(element_size));

  TNode<JSFunction> const default_constructor = CAST(LoadContextElement(
      LoadNativeContext(context), Context::ARRAY_BUFFER_FUN_INDEX));

  Label construct(this), if_detached(this), if_notdetached(this),
      check_for_sab(this), if_buffernotshared(this), check_prototype(this),
      done(this);
  TVARIABLE(JSReceiver, buffer_constructor, default_constructor);

  TNode<JSArrayBuffer> source_buffer = LoadObjectField<JSArrayBuffer>(
      typed_array, JSArrayBufferView::kBufferOffset);
  Branch(IsDetachedBuffer(source_buffer), &if_detached, &if_notdetached);

  // TODO(petermarshall): Throw on detached typedArray.
  TVARIABLE(Smi, source_length);
  BIND(&if_detached);
  source_length = SmiConstant(0);
  Goto(&check_for_sab);

  BIND(&if_notdetached);
  source_length =
      CAST(LoadObjectField(typed_array, JSTypedArray::kLengthOffset));
  Goto(&check_for_sab);

  // The spec requires that constructing a typed array using a SAB-backed typed
  // array use the ArrayBuffer constructor, not the species constructor. See
  // https://tc39.github.io/ecma262/#sec-typedarray-typedarray.
  BIND(&check_for_sab);
  TNode<Uint32T> bitfield =
      LoadObjectField<Uint32T>(source_buffer, JSArrayBuffer::kBitFieldOffset);
  Branch(IsSetWord32<JSArrayBuffer::IsShared>(bitfield), &construct,
         &if_buffernotshared);

  BIND(&if_buffernotshared);
  {
    buffer_constructor =
        CAST(SpeciesConstructor(context, source_buffer, default_constructor));
    // TODO(petermarshall): Throw on detached typedArray.
    GotoIfNot(IsDetachedBuffer(source_buffer), &construct);
    source_length = SmiConstant(0);
    Goto(&construct);
  }

  BIND(&construct);
  {
    ConstructByArrayLike(context, holder, typed_array, source_length,
                         element_size);
    Node* proto =
        GetProperty(context, buffer_constructor, PrototypeStringConstant());
    // TODO(petermarshall): Correct for realm as per 9.1.14 step 4.
    TNode<JSArrayBuffer> buffer = LoadObjectField<JSArrayBuffer>(
        holder, JSArrayBufferView::kBufferOffset);
    CallRuntime(Runtime::kInternalSetPrototype, context, buffer, proto);

    Goto(&done);
  }

  BIND(&done);
}

Node* TypedArrayBuiltinsAssembler::LoadDataPtr(Node* typed_array) {
  CSA_ASSERT(this, IsJSTypedArray(typed_array));
  Node* elements = LoadElements(typed_array);
  CSA_ASSERT(this, IsFixedTypedArray(elements));
  Node* base_pointer = BitcastTaggedToWord(
      LoadObjectField(elements, FixedTypedArrayBase::kBasePointerOffset));
  Node* external_pointer = BitcastTaggedToWord(
      LoadObjectField(elements, FixedTypedArrayBase::kExternalPointerOffset));
  return IntPtrAdd(base_pointer, external_pointer);
}

TNode<BoolT> TypedArrayBuiltinsAssembler::ByteLengthIsValid(
    TNode<Number> byte_length) {
  Label smi(this), done(this);
  TVARIABLE(BoolT, is_valid);
  GotoIf(TaggedIsSmi(byte_length), &smi);

  TNode<Float64T> float_value = LoadHeapNumberValue(CAST(byte_length));
  TNode<Float64T> max_byte_length_double =
      Float64Constant(FixedTypedArrayBase::kMaxByteLength);
  is_valid = Float64LessThanOrEqual(float_value, max_byte_length_double);
  Goto(&done);

  BIND(&smi);
  TNode<IntPtrT> max_byte_length =
      IntPtrConstant(FixedTypedArrayBase::kMaxByteLength);
  is_valid =
      UintPtrLessThanOrEqual(SmiUntag(CAST(byte_length)), max_byte_length);
  Goto(&done);

  BIND(&done);
  return is_valid;
}

void TypedArrayBuiltinsAssembler::ConstructByArrayLike(
    TNode<Context> context, TNode<JSTypedArray> holder,
    TNode<HeapObject> array_like, TNode<Object> initial_length,
    TNode<Smi> element_size) {
  Node* initialize = FalseConstant();

  Label invalid_length(this), fill(this), fast_copy(this), done(this);

  // The caller has looked up length on array_like, which is observable.
  Node* length = ToSmiLength(initial_length, context, &invalid_length);

  CallBuiltin(Builtins::kTypedArrayInitialize, context, holder, length,
              element_size, initialize);
  GotoIf(SmiNotEqual(length, SmiConstant(0)), &fill);
  Goto(&done);

  BIND(&fill);
  TNode<Int32T> holder_kind = LoadMapElementsKind(LoadMap(holder));
  TNode<Int32T> source_kind = LoadMapElementsKind(LoadMap(array_like));
  GotoIf(Word32Equal(holder_kind, source_kind), &fast_copy);

  // Copy using the elements accessor.
  CallRuntime(Runtime::kTypedArrayCopyElements, context, holder, array_like,
              length);
  Goto(&done);

  BIND(&fast_copy);
  {
    Node* holder_data_ptr = LoadDataPtr(holder);
    Node* source_data_ptr = LoadDataPtr(array_like);

    // Calculate the byte length. We shouldn't be trying to copy if the typed
    // array was neutered.
    CSA_ASSERT(this, SmiNotEqual(length, SmiConstant(0)));
    CSA_ASSERT(this, Word32Equal(IsDetachedBuffer(LoadObjectField(
                                     array_like, JSTypedArray::kBufferOffset)),
                                 Int32Constant(0)));

    TNode<Number> byte_length = SmiMul(length, element_size);
    CSA_ASSERT(this, ByteLengthIsValid(byte_length));
    TNode<UintPtrT> byte_length_intptr =
        ChangeNonnegativeNumberToUintPtr(byte_length);
    CSA_ASSERT(this, UintPtrLessThanOrEqual(
                         byte_length_intptr,
                         IntPtrConstant(FixedTypedArrayBase::kMaxByteLength)));

    Node* memcpy =
        ExternalConstant(ExternalReference::libc_memcpy_function(isolate()));
    CallCFunction3(MachineType::AnyTagged(), MachineType::Pointer(),
                   MachineType::Pointer(), MachineType::UintPtr(), memcpy,
                   holder_data_ptr, source_data_ptr, byte_length_intptr);
    Goto(&done);
  }

  BIND(&invalid_length);
  {
    CallRuntime(Runtime::kThrowRangeError, context,
                SmiConstant(MessageTemplate::kInvalidTypedArrayLength),
                initial_length);
    Unreachable();
  }

  BIND(&done);
}

TF_BUILTIN(TypedArrayConstructByArrayLike, TypedArrayBuiltinsAssembler) {
  TNode<JSTypedArray> holder = CAST(Parameter(Descriptor::kHolder));
  TNode<HeapObject> array_like = CAST(Parameter(Descriptor::kArrayLike));
  TNode<Object> initial_length = CAST(Parameter(Descriptor::kLength));
  TNode<Smi> element_size = CAST(Parameter(Descriptor::kElementSize));
  CSA_ASSERT(this, TaggedIsSmi(element_size));
  TNode<Context> context = CAST(Parameter(Descriptor::kContext));

  ConstructByArrayLike(context, holder, array_like, initial_length,
                       element_size);
  Return(UndefinedConstant());
}

TF_BUILTIN(TypedArrayConstructor, TypedArrayBuiltinsAssembler) {
  TNode<Context> context = CAST(Parameter(BuiltinDescriptor::kContext));

  // If NewTarget is undefined, throw a TypeError exception.
  Node* target = LoadFromFrame(StandardFrameConstants::kFunctionOffset,
                               MachineType::TaggedPointer());
  Node* shared = LoadObjectField(target, JSFunction::kSharedFunctionInfoOffset);
  Node* name = LoadObjectField(shared, SharedFunctionInfo::kNameOffset);
  ThrowTypeError(context, MessageTemplate::kConstructorNotFunction, name);
}

TF_BUILTIN(TypedArrayConstructor_ConstructStub, TypedArrayBuiltinsAssembler) {
  Label if_arg1isbuffer(this), if_arg1istypedarray(this),
      if_arg1isreceiver(this), if_iteratorundefined(this),
      if_arg1isnumber(this), done(this);

  TNode<Object> new_target = CAST(Parameter(BuiltinDescriptor::kNewTarget));
  CSA_ASSERT(this, IsNotUndefined(new_target));

  Node* argc =
      ChangeInt32ToIntPtr(Parameter(BuiltinDescriptor::kArgumentsCount));
  CodeStubArguments args(this, argc);
  TNode<Object> arg1 = CAST(args.GetOptionalArgumentValue(0));
  TNode<Object> arg2 = CAST(args.GetOptionalArgumentValue(1));
  TNode<Object> arg3 = CAST(args.GetOptionalArgumentValue(2));
  TNode<Context> context = CAST(Parameter(BuiltinDescriptor::kContext));

  Node* target = LoadFromFrame(StandardFrameConstants::kFunctionOffset,
                               MachineType::TaggedPointer());
  ConstructorBuiltinsAssembler constructor_assembler(this->state());
  TNode<JSTypedArray> holder = CAST(
      constructor_assembler.EmitFastNewObject(context, target, new_target));

  TNode<Smi> element_size =
      SmiTag(GetTypedArrayElementSize(LoadElementsKind(holder)));

  GotoIf(TaggedIsSmi(arg1), &if_arg1isnumber);
  GotoIf(IsJSArrayBuffer(arg1), &if_arg1isbuffer);
  GotoIf(IsJSTypedArray(arg1), &if_arg1istypedarray);
  GotoIf(IsJSReceiver(arg1), &if_arg1isreceiver);
  Goto(&if_arg1isnumber);

  BIND(&if_arg1isbuffer);
  ConstructByArrayBuffer(context, holder, CAST(arg1), arg2, arg3, element_size);
  Goto(&done);

  BIND(&if_arg1istypedarray);
  TNode<JSTypedArray> typed_array = CAST(arg1);
  ConstructByTypedArray(context, holder, typed_array, element_size);
  Goto(&done);

  BIND(&if_arg1isreceiver);
  {
    // Get iterator symbol
    TNode<Object> iteratorFn = CAST(
        GetProperty(context, arg1, isolate()->factory()->iterator_symbol()));
    GotoIf(IsUndefined(iteratorFn), &if_iteratorundefined);

    // Call ConstructByIterable.
    // TODO(petermarshall): Port ConstructByIterable to CSA.
    Node* construct_iterable =
        LoadContextElement(LoadNativeContext(context),
                           Context::TYPED_ARRAY_CONSTRUCT_BY_ITERABLE_INDEX);
    CallJS(CodeFactory::Call(isolate()), context, construct_iterable,
           UndefinedConstant(), holder, arg1, iteratorFn, element_size);
    Goto(&done);

    BIND(&if_iteratorundefined);
    {
      TNode<HeapObject> array_like = CAST(arg1);
      TNode<Object> initial_length =
          CAST(GetProperty(context, arg1, LengthStringConstant()));

      ConstructByArrayLike(context, holder, array_like, initial_length,
                           element_size);
      Goto(&done);
    }
  }

  // First arg was a number or fell through and will be treated as a number.
  BIND(&if_arg1isnumber);
  ConstructByLength(context, holder, arg1, element_size);
  Goto(&done);

  BIND(&done);
  args.PopAndReturn(holder);
}

void TypedArrayBuiltinsAssembler::GenerateTypedArrayPrototypeGetter(
    Node* context, Node* receiver, const char* method_name, int object_offset) {
  // Check if the {receiver} is actually a JSTypedArray.
  Label receiver_is_incompatible(this, Label::kDeferred);
  GotoIf(TaggedIsSmi(receiver), &receiver_is_incompatible);
  GotoIfNot(HasInstanceType(receiver, JS_TYPED_ARRAY_TYPE),
            &receiver_is_incompatible);

  // Check if the {receiver}'s JSArrayBuffer was neutered.
  Node* receiver_buffer =
      LoadObjectField(receiver, JSTypedArray::kBufferOffset);
  Label if_receiverisneutered(this, Label::kDeferred);
  GotoIf(IsDetachedBuffer(receiver_buffer), &if_receiverisneutered);
  Return(LoadObjectField(receiver, object_offset));

  BIND(&if_receiverisneutered);
  {
    // The {receiver}s buffer was neutered, default to zero.
    Return(SmiConstant(0));
  }

  BIND(&receiver_is_incompatible);
  {
    // The {receiver} is not a valid JSTypedArray.
    CallRuntime(Runtime::kThrowIncompatibleMethodReceiver, context,
                StringConstant(method_name), receiver);
    Unreachable();
  }
}

// ES6 #sec-get-%typedarray%.prototype.bytelength
TF_BUILTIN(TypedArrayPrototypeByteLength, TypedArrayBuiltinsAssembler) {
  Node* context = Parameter(Descriptor::kContext);
  Node* receiver = Parameter(Descriptor::kReceiver);
  GenerateTypedArrayPrototypeGetter(context, receiver,
                                    "get TypedArray.prototype.byteLength",
                                    JSTypedArray::kByteLengthOffset);
}

// ES6 #sec-get-%typedarray%.prototype.byteoffset
TF_BUILTIN(TypedArrayPrototypeByteOffset, TypedArrayBuiltinsAssembler) {
  Node* context = Parameter(Descriptor::kContext);
  Node* receiver = Parameter(Descriptor::kReceiver);
  GenerateTypedArrayPrototypeGetter(context, receiver,
                                    "get TypedArray.prototype.byteOffset",
                                    JSTypedArray::kByteOffsetOffset);
}

// ES6 #sec-get-%typedarray%.prototype.length
TF_BUILTIN(TypedArrayPrototypeLength, TypedArrayBuiltinsAssembler) {
  Node* context = Parameter(Descriptor::kContext);
  Node* receiver = Parameter(Descriptor::kReceiver);
  GenerateTypedArrayPrototypeGetter(context, receiver,
                                    "get TypedArray.prototype.length",
                                    JSTypedArray::kLengthOffset);
}

TNode<Word32T> TypedArrayBuiltinsAssembler::IsUint8ElementsKind(
    TNode<Word32T> kind) {
  return Word32Or(Word32Equal(kind, Int32Constant(UINT8_ELEMENTS)),
                  Word32Equal(kind, Int32Constant(UINT8_CLAMPED_ELEMENTS)));
}

TNode<Word32T> TypedArrayBuiltinsAssembler::LoadElementsKind(
    TNode<Object> typed_array) {
  CSA_ASSERT(this, IsJSTypedArray(typed_array));
  return LoadMapElementsKind(LoadMap(CAST(typed_array)));
}

TNode<IntPtrT> TypedArrayBuiltinsAssembler::GetTypedArrayElementSize(
    TNode<Word32T> elements_kind) {
  TVARIABLE(IntPtrT, element_size);
  Label next(this), if_unknown_type(this, Label::kDeferred);

  size_t const kTypedElementsKindCount = LAST_FIXED_TYPED_ARRAY_ELEMENTS_KIND -
                                         FIRST_FIXED_TYPED_ARRAY_ELEMENTS_KIND +
                                         1;

  int32_t elements_kinds[kTypedElementsKindCount] = {
#define TYPED_ARRAY_CASE(Type, type, TYPE, ctype, size) TYPE##_ELEMENTS,
      TYPED_ARRAYS(TYPED_ARRAY_CASE)
#undef TYPED_ARRAY_CASE
  };

#define TYPED_ARRAY_CASE(Type, type, TYPE, ctype, size) \
  Label if_##type##array(this);
  TYPED_ARRAYS(TYPED_ARRAY_CASE)
#undef TYPED_ARRAY_CASE

  Label* elements_kind_labels[kTypedElementsKindCount] = {
#define TYPED_ARRAY_CASE(Type, type, TYPE, ctype, size) &if_##type##array,
      TYPED_ARRAYS(TYPED_ARRAY_CASE)
#undef TYPED_ARRAY_CASE
  };

  Switch(elements_kind, &if_unknown_type, elements_kinds, elements_kind_labels,
         kTypedElementsKindCount);

#define TYPED_ARRAY_CASE(Type, type, TYPE, ctype, size) \
  BIND(&if_##type##array);                              \
  {                                                     \
    element_size = IntPtrConstant(size);                \
    Goto(&next);                                        \
  }
  TYPED_ARRAYS(TYPED_ARRAY_CASE)
#undef TYPED_ARRAY_CASE

  BIND(&if_unknown_type);
  {
    element_size = IntPtrConstant(0);
    Goto(&next);
  }
  BIND(&next);
  return element_size;
}

TNode<Object> TypedArrayBuiltinsAssembler::GetDefaultConstructor(
    TNode<Context> context, TNode<JSTypedArray> exemplar) {
  TVARIABLE(IntPtrT, context_slot);
  TNode<Word32T> elements_kind = LoadElementsKind(exemplar);

  Label next(this), constructor_found(this),
      if_unknown_type(this, Label::kDeferred);

  size_t const kTypedElementsKindCount = LAST_FIXED_TYPED_ARRAY_ELEMENTS_KIND -
                                         FIRST_FIXED_TYPED_ARRAY_ELEMENTS_KIND +
                                         1;

  int32_t elements_kinds[kTypedElementsKindCount] = {
#define TYPED_ARRAY_CASE(Type, type, TYPE, ctype, size) TYPE##_ELEMENTS,
      TYPED_ARRAYS(TYPED_ARRAY_CASE)
#undef TYPED_ARRAY_CASE
  };

#define TYPED_ARRAY_CASE(Type, type, TYPE, ctype, size) \
  Label if_##type##array(this);
  TYPED_ARRAYS(TYPED_ARRAY_CASE)
#undef TYPED_ARRAY_CASE

  Label* elements_kind_labels[kTypedElementsKindCount] = {
#define TYPED_ARRAY_CASE(Type, type, TYPE, ctype, size) &if_##type##array,
      TYPED_ARRAYS(TYPED_ARRAY_CASE)
#undef TYPED_ARRAY_CASE
  };

  Switch(elements_kind, &if_unknown_type, elements_kinds, elements_kind_labels,
         kTypedElementsKindCount);

#define TYPED_ARRAY_CASE(Type, type, TYPE, ctype, size)             \
  BIND(&if_##type##array);                                          \
  {                                                                 \
    context_slot = IntPtrConstant(Context::TYPE##_ARRAY_FUN_INDEX); \
    Goto(&next);                                                    \
  }
  TYPED_ARRAYS(TYPED_ARRAY_CASE)
#undef TYPED_ARRAY_CASE

  BIND(&if_unknown_type);
  Unreachable();

  BIND(&next);
  return LoadContextElement(LoadNativeContext(context), context_slot);
}

TNode<Object> TypedArrayBuiltinsAssembler::TypedArraySpeciesConstructor(
    TNode<Context> context, TNode<JSTypedArray> exemplar) {
  TVARIABLE(Object, var_constructor);
  Label slow(this), done(this);

  // Let defaultConstructor be the intrinsic object listed in column one of
  // Table 52 for exemplar.[[TypedArrayName]].
  TNode<Object> default_constructor = GetDefaultConstructor(context, exemplar);

  var_constructor = default_constructor;
  Node* map = LoadMap(exemplar);
  GotoIfNot(IsPrototypeTypedArrayPrototype(context, map), &slow);
  Branch(IsSpeciesProtectorCellInvalid(), &slow, &done);

  BIND(&slow);
  var_constructor =
      CAST(SpeciesConstructor(context, exemplar, default_constructor));
  Goto(&done);

  BIND(&done);
  return var_constructor;
}

TNode<JSTypedArray> TypedArrayBuiltinsAssembler::SpeciesCreateByArrayBuffer(
    TNode<Context> context, TNode<JSTypedArray> exemplar,
    TNode<JSArrayBuffer> buffer, TNode<Number> byte_offset, TNode<Smi> len,
    const char* method_name) {
  // Let constructor be ? SpeciesConstructor(exemplar, defaultConstructor).
  TNode<Object> constructor = TypedArraySpeciesConstructor(context, exemplar);

  // Let newTypedArray be ? Construct(constructor, argumentList).
  TNode<Object> new_object =
      CAST(ConstructJS(CodeFactory::Construct(isolate()), context, constructor,
                       buffer, byte_offset, len));

  // Perform ? ValidateTypedArray(newTypedArray).
  return ValidateTypedArray(context, new_object, method_name);
}

TNode<JSTypedArray> TypedArrayBuiltinsAssembler::SpeciesCreateByLength(
    TNode<Context> context, TNode<JSTypedArray> exemplar, TNode<Smi> len,
    const char* method_name) {
  CSA_ASSERT(this, TaggedIsPositiveSmi(len));

  // Let constructor be ? SpeciesConstructor(exemplar, defaultConstructor).
  TNode<Object> constructor = TypedArraySpeciesConstructor(context, exemplar);
  CSA_ASSERT(this, IsJSFunction(constructor));

  // Let newTypedArray be ? Construct(constructor, argumentList).
  TNode<Object> new_object = CAST(ConstructJS(CodeFactory::Construct(isolate()),
                                              context, constructor, len));

  // Perform ? ValidateTypedArray(newTypedArray).
  TNode<JSTypedArray> new_typed_array =
      ValidateTypedArray(context, new_object, method_name);

  // If newTypedArray.[[ArrayLength]] < argumentList[0], throw a TypeError
  // exception.
  Label if_length_is_not_short(this);
  TNode<Smi> new_length =
      LoadObjectField<Smi>(new_typed_array, JSTypedArray::kLengthOffset);
  GotoIfNot(SmiLessThan(new_length, len), &if_length_is_not_short);
  ThrowTypeError(context, MessageTemplate::kNotTypedArray);

  BIND(&if_length_is_not_short);
  return new_typed_array;
}

TNode<JSArrayBuffer> TypedArrayBuiltinsAssembler::GetBuffer(
    TNode<Context> context, TNode<JSTypedArray> array) {
  Label call_runtime(this), done(this);
  TVARIABLE(Object, var_result);

  TNode<Object> buffer = LoadObjectField(array, JSTypedArray::kBufferOffset);
  GotoIf(IsDetachedBuffer(buffer), &call_runtime);
  TNode<UintPtrT> backing_store = LoadObjectField<UintPtrT>(
      CAST(buffer), JSArrayBuffer::kBackingStoreOffset);
  GotoIf(WordEqual(backing_store, IntPtrConstant(0)), &call_runtime);
  var_result = buffer;
  Goto(&done);

  BIND(&call_runtime);
  {
    var_result = CallRuntime(Runtime::kTypedArrayGetBuffer, context, array);
    Goto(&done);
  }

  BIND(&done);
  return CAST(var_result);
}

TNode<JSTypedArray> TypedArrayBuiltinsAssembler::ValidateTypedArray(
    TNode<Context> context, TNode<Object> obj, const char* method_name) {
  Label validation_done(this);

  // If it is not a typed array, throw
  ThrowIfNotInstanceType(context, obj, JS_TYPED_ARRAY_TYPE, method_name);

  // If the typed array's buffer is detached, throw
  TNode<Object> buffer =
      LoadObjectField(CAST(obj), JSTypedArray::kBufferOffset);
  GotoIfNot(IsDetachedBuffer(buffer), &validation_done);
  ThrowTypeError(context, MessageTemplate::kDetachedOperation, method_name);

  BIND(&validation_done);
  return CAST(obj);
}

void TypedArrayBuiltinsAssembler::SetTypedArraySource(
    TNode<Context> context, TNode<JSTypedArray> source,
    TNode<JSTypedArray> target, TNode<IntPtrT> offset, Label* call_runtime,
    Label* if_source_too_large) {
  CSA_ASSERT(this, Word32BinaryNot(IsDetachedBuffer(
                       LoadObjectField(source, JSTypedArray::kBufferOffset))));
  CSA_ASSERT(this, Word32BinaryNot(IsDetachedBuffer(
                       LoadObjectField(target, JSTypedArray::kBufferOffset))));
  CSA_ASSERT(this, IntPtrGreaterThanOrEqual(offset, IntPtrConstant(0)));
  CSA_ASSERT(this,
             IntPtrLessThanOrEqual(offset, IntPtrConstant(Smi::kMaxValue)));

  // Check for possible range errors.

  TNode<IntPtrT> source_length =
      LoadAndUntagObjectField(source, JSTypedArray::kLengthOffset);
  TNode<IntPtrT> target_length =
      LoadAndUntagObjectField(target, JSTypedArray::kLengthOffset);
  TNode<IntPtrT> required_target_length = IntPtrAdd(source_length, offset);

  GotoIf(IntPtrGreaterThan(required_target_length, target_length),
         if_source_too_large);

  // Grab pointers and byte lengths we need later on.

  TNode<IntPtrT> target_data_ptr = UncheckedCast<IntPtrT>(LoadDataPtr(target));
  TNode<IntPtrT> source_data_ptr = UncheckedCast<IntPtrT>(LoadDataPtr(source));

  TNode<Word32T> source_el_kind = LoadElementsKind(source);
  TNode<Word32T> target_el_kind = LoadElementsKind(target);

  TNode<IntPtrT> source_el_size = GetTypedArrayElementSize(source_el_kind);
  TNode<IntPtrT> target_el_size = GetTypedArrayElementSize(target_el_kind);

  // A note on byte lengths: both source- and target byte lengths must be valid,
  // i.e. it must be possible to allocate an array of the given length. That
  // means we're safe from overflows in the following multiplication.
  TNode<IntPtrT> source_byte_length = IntPtrMul(source_length, source_el_size);
  CSA_ASSERT(this,
             UintPtrGreaterThanOrEqual(source_byte_length, IntPtrConstant(0)));

  Label call_memmove(this), fast_c_call(this), out(this);

  // A fast memmove call can be used when the source and target types are are
  // the same or either Uint8 or Uint8Clamped.
  GotoIf(Word32Equal(source_el_kind, target_el_kind), &call_memmove);
  GotoIfNot(IsUint8ElementsKind(source_el_kind), &fast_c_call);
  Branch(IsUint8ElementsKind(target_el_kind), &call_memmove, &fast_c_call);

  BIND(&call_memmove);
  {
    TNode<IntPtrT> target_start =
        IntPtrAdd(target_data_ptr, IntPtrMul(offset, target_el_size));
    CallCMemmove(target_start, source_data_ptr, source_byte_length);
    Goto(&out);
  }

  BIND(&fast_c_call);
  {
    CSA_ASSERT(
        this, UintPtrGreaterThanOrEqual(
                  IntPtrMul(target_length, target_el_size), IntPtrConstant(0)));

    TNode<IntPtrT> source_length =
        LoadAndUntagObjectField(source, JSTypedArray::kLengthOffset);
    CallCCopyTypedArrayElementsToTypedArray(source, target, source_length,
                                            offset);
    Goto(&out);
  }

  BIND(&out);
}

void TypedArrayBuiltinsAssembler::SetJSArraySource(
    TNode<Context> context, TNode<JSArray> source, TNode<JSTypedArray> target,
    TNode<IntPtrT> offset, Label* call_runtime, Label* if_source_too_large) {
  CSA_ASSERT(this, IsFastJSArray(source, context));
  CSA_ASSERT(this, IntPtrGreaterThanOrEqual(offset, IntPtrConstant(0)));
  CSA_ASSERT(this,
             IntPtrLessThanOrEqual(offset, IntPtrConstant(Smi::kMaxValue)));

  TNode<IntPtrT> source_length = SmiUntag(LoadFastJSArrayLength(source));
  TNode<IntPtrT> target_length =
      LoadAndUntagObjectField(target, JSTypedArray::kLengthOffset);

  // Maybe out of bounds?
  GotoIf(IntPtrGreaterThan(IntPtrAdd(source_length, offset), target_length),
         if_source_too_large);

  // Nothing to do if {source} is empty.
  Label out(this), fast_c_call(this);
  GotoIf(IntPtrEqual(source_length, IntPtrConstant(0)), &out);

  // Dispatch based on the source elements kind.
  {
    // These are the supported elements kinds in TryCopyElementsFastNumber.
    int32_t values[] = {
        PACKED_SMI_ELEMENTS, HOLEY_SMI_ELEMENTS, PACKED_DOUBLE_ELEMENTS,
        HOLEY_DOUBLE_ELEMENTS,
    };
    Label* labels[] = {
        &fast_c_call, &fast_c_call, &fast_c_call, &fast_c_call,
    };
    STATIC_ASSERT(arraysize(values) == arraysize(labels));

    TNode<Int32T> source_elements_kind = LoadMapElementsKind(LoadMap(source));
    Switch(source_elements_kind, call_runtime, values, labels,
           arraysize(values));
  }

  BIND(&fast_c_call);
  CallCCopyFastNumberJSArrayElementsToTypedArray(context, source, target,
                                                 source_length, offset);
  Goto(&out);
  BIND(&out);
}

void TypedArrayBuiltinsAssembler::CallCMemmove(TNode<IntPtrT> dest_ptr,
                                               TNode<IntPtrT> src_ptr,
                                               TNode<IntPtrT> byte_length) {
  TNode<ExternalReference> memmove =
      ExternalConstant(ExternalReference::libc_memmove_function(isolate()));
  CallCFunction3(MachineType::AnyTagged(), MachineType::Pointer(),
                 MachineType::Pointer(), MachineType::UintPtr(), memmove,
                 dest_ptr, src_ptr, byte_length);
}

void TypedArrayBuiltinsAssembler::
    CallCCopyFastNumberJSArrayElementsToTypedArray(TNode<Context> context,
                                                   TNode<JSArray> source,
                                                   TNode<JSTypedArray> dest,
                                                   TNode<IntPtrT> source_length,
                                                   TNode<IntPtrT> offset) {
  TNode<ExternalReference> f = ExternalConstant(
      ExternalReference::copy_fast_number_jsarray_elements_to_typed_array(
          isolate()));
  CallCFunction5(MachineType::AnyTagged(), MachineType::AnyTagged(),
                 MachineType::AnyTagged(), MachineType::AnyTagged(),
                 MachineType::UintPtr(), MachineType::UintPtr(), f, context,
                 source, dest, source_length, offset);
}

void TypedArrayBuiltinsAssembler::CallCCopyTypedArrayElementsToTypedArray(
    TNode<JSTypedArray> source, TNode<JSTypedArray> dest,
    TNode<IntPtrT> source_length, TNode<IntPtrT> offset) {
  TNode<ExternalReference> f = ExternalConstant(
      ExternalReference::copy_typed_array_elements_to_typed_array(isolate()));
  CallCFunction4(MachineType::AnyTagged(), MachineType::AnyTagged(),
                 MachineType::AnyTagged(), MachineType::UintPtr(),
                 MachineType::UintPtr(), f, source, dest, source_length,
                 offset);
}

// ES #sec-get-%typedarray%.prototype.set
TF_BUILTIN(TypedArrayPrototypeSet, TypedArrayBuiltinsAssembler) {
  TNode<Context> context = CAST(Parameter(BuiltinDescriptor::kContext));
  CodeStubArguments args(
      this, ChangeInt32ToIntPtr(Parameter(BuiltinDescriptor::kArgumentsCount)));

  Label if_source_is_typed_array(this), if_source_is_fast_jsarray(this),
      if_offset_is_out_of_bounds(this, Label::kDeferred),
      if_source_too_large(this, Label::kDeferred),
      if_typed_array_is_neutered(this, Label::kDeferred),
      if_receiver_is_not_typedarray(this, Label::kDeferred);

  // Check the receiver is a typed array.
  TNode<Object> receiver = args.GetReceiver();
  GotoIf(TaggedIsSmi(receiver), &if_receiver_is_not_typedarray);
  GotoIfNot(IsJSTypedArray(receiver), &if_receiver_is_not_typedarray);

  // Normalize offset argument (using ToInteger) and handle heap number cases.
  TNode<Object> offset = args.GetOptionalArgumentValue(1, SmiConstant(0));
  TNode<Number> offset_num =
      ToInteger_Inline(context, offset, kTruncateMinusZero);

  // Since ToInteger always returns a Smi if the given value is within Smi
  // range, and the only corner case of -0.0 has already been truncated to 0.0,
  // we can simply throw unless the offset is a non-negative Smi.
  // TODO(jgruber): It's an observable spec violation to throw here if
  // {offset_num} is a positive number outside the Smi range. Per spec, we need
  // to check for detached buffers and call the observable ToObject/ToLength
  // operations first.
  GotoIfNot(TaggedIsPositiveSmi(offset_num), &if_offset_is_out_of_bounds);
  TNode<Smi> offset_smi = CAST(offset_num);

  // Check the receiver is not neutered.
  TNode<Object> receiver_buffer =
      LoadObjectField(CAST(receiver), JSTypedArray::kBufferOffset);
  GotoIf(IsDetachedBuffer(receiver_buffer), &if_typed_array_is_neutered);

  // Check the source argument is valid and whether a fast path can be taken.
  Label call_runtime(this);
  TNode<Object> source = args.GetOptionalArgumentValue(0);
  GotoIf(TaggedIsSmi(source), &call_runtime);
  GotoIf(IsJSTypedArray(source), &if_source_is_typed_array);
  BranchIfFastJSArray(source, context, &if_source_is_fast_jsarray,
                      &call_runtime);

  // Fast path for a typed array source argument.
  BIND(&if_source_is_typed_array);
  {
    // Check the source argument is not neutered.
    TNode<Object> source_buffer =
        LoadObjectField(CAST(source), JSTypedArray::kBufferOffset);
    GotoIf(IsDetachedBuffer(source_buffer), &if_typed_array_is_neutered);

    SetTypedArraySource(context, CAST(source), CAST(receiver),
                        SmiUntag(offset_smi), &call_runtime,
                        &if_source_too_large);
    args.PopAndReturn(UndefinedConstant());
  }

  // Fast path for a fast JSArray source argument.
  BIND(&if_source_is_fast_jsarray);
  {
    SetJSArraySource(context, CAST(source), CAST(receiver),
                     SmiUntag(offset_smi), &call_runtime, &if_source_too_large);
    args.PopAndReturn(UndefinedConstant());
  }

  BIND(&call_runtime);
  args.PopAndReturn(CallRuntime(Runtime::kTypedArraySet, context, receiver,
                                source, offset_smi));

  BIND(&if_offset_is_out_of_bounds);
  ThrowRangeError(context, MessageTemplate::kTypedArraySetOffsetOutOfBounds);

  BIND(&if_source_too_large);
  ThrowRangeError(context, MessageTemplate::kTypedArraySetSourceTooLarge);

  BIND(&if_typed_array_is_neutered);
  ThrowTypeError(context, MessageTemplate::kDetachedOperation,
                 "%TypedArray%.prototype.set");

  BIND(&if_receiver_is_not_typedarray);
  ThrowTypeError(context, MessageTemplate::kNotTypedArray);
}

// ES %TypedArray%.prototype.slice
TF_BUILTIN(TypedArrayPrototypeSlice, TypedArrayBuiltinsAssembler) {
  const char* method_name = "%TypedArray%.prototype.slice";
  Label call_runtime(this), call_memmove(this), if_count_is_not_zero(this),
      if_typed_array_is_neutered(this, Label::kDeferred);

  TNode<Context> context = CAST(Parameter(BuiltinDescriptor::kContext));
  CodeStubArguments args(
      this, ChangeInt32ToIntPtr(Parameter(BuiltinDescriptor::kArgumentsCount)));

  TNode<Object> receiver = args.GetReceiver();
  TNode<JSTypedArray> source =
      ValidateTypedArray(context, receiver, method_name);

  TNode<Smi> source_length =
      LoadObjectField<Smi>(source, JSTypedArray::kLengthOffset);

  // Convert start offset argument to integer, and calculate relative offset.
  TNode<Object> start = args.GetOptionalArgumentValue(0, SmiConstant(0));
  TNode<Smi> start_index =
      ConvertToRelativeIndex(context, start, source_length);

  // Convert end offset argument to integer, and calculate relative offset.
  // If end offset is not given or undefined is given, set source_length to
  // "end_index".
  TNode<Object> end = args.GetOptionalArgumentValue(1, UndefinedConstant());
  TNode<Smi> end_index = Select<Smi>(
      IsUndefined(end), [=] { return source_length; },
      [=] { return ConvertToRelativeIndex(context, end, source_length); },
      MachineRepresentation::kTagged);

  // Create a result array by invoking TypedArraySpeciesCreate.
  TNode<Smi> count = SmiMax(SmiSub(end_index, start_index), SmiConstant(0));
  TNode<JSTypedArray> result_array =
      SpeciesCreateByLength(context, source, count, method_name);

  // If count is zero, return early.
  GotoIf(SmiGreaterThan(count, SmiConstant(0)), &if_count_is_not_zero);
  args.PopAndReturn(result_array);

  BIND(&if_count_is_not_zero);
  // Check the source array is neutered or not. We don't need to check if the
  // result array is neutered or not since TypedArraySpeciesCreate checked it.
  CSA_ASSERT(this, Word32BinaryNot(IsDetachedBuffer(LoadObjectField(
                       result_array, JSTypedArray::kBufferOffset))));
  TNode<Object> receiver_buffer =
      LoadObjectField(CAST(receiver), JSTypedArray::kBufferOffset);
  GotoIf(IsDetachedBuffer(receiver_buffer), &if_typed_array_is_neutered);

  // result_array could be a different type from source or share the same
  // buffer with the source because of custom species constructor.
  // If the types of source and result array are the same and they are not
  // sharing the same buffer, use memmove.
  TNode<Word32T> source_el_kind = LoadElementsKind(source);
  TNode<Word32T> target_el_kind = LoadElementsKind(result_array);
  GotoIfNot(Word32Equal(source_el_kind, target_el_kind), &call_runtime);

  TNode<Object> target_buffer =
      LoadObjectField(result_array, JSTypedArray::kBufferOffset);
  Branch(WordEqual(receiver_buffer, target_buffer), &call_runtime,
         &call_memmove);

  BIND(&call_memmove);
  {
    GotoIfForceSlowPath(&call_runtime);

    TNode<IntPtrT> target_data_ptr =
        UncheckedCast<IntPtrT>(LoadDataPtr(result_array));
    TNode<IntPtrT> source_data_ptr =
        UncheckedCast<IntPtrT>(LoadDataPtr(source));

    TNode<IntPtrT> source_el_size = GetTypedArrayElementSize(source_el_kind);
    TNode<IntPtrT> source_start_bytes =
        IntPtrMul(SmiToWord(start_index), source_el_size);
    TNode<IntPtrT> source_start =
        IntPtrAdd(source_data_ptr, source_start_bytes);

    TNode<IntPtrT> count_bytes = IntPtrMul(SmiToWord(count), source_el_size);

#ifdef DEBUG
    TNode<IntPtrT> target_byte_length =
        LoadAndUntagObjectField(result_array, JSTypedArray::kByteLengthOffset);
    CSA_ASSERT(this, IntPtrLessThanOrEqual(count_bytes, target_byte_length));

    TNode<IntPtrT> source_byte_length =
        LoadAndUntagObjectField(source, JSTypedArray::kByteLengthOffset);
    TNode<IntPtrT> source_size_in_bytes =
        IntPtrSub(source_byte_length, source_start_bytes);
    CSA_ASSERT(this, IntPtrLessThanOrEqual(count_bytes, source_size_in_bytes));
#endif  // DEBUG

    CallCMemmove(target_data_ptr, source_start, count_bytes);
    args.PopAndReturn(result_array);
  }

  BIND(&call_runtime);
  args.PopAndReturn(CallRuntime(Runtime::kTypedArraySlice, context, source,
                                start_index, end_index, result_array));

  BIND(&if_typed_array_is_neutered);
  ThrowTypeError(context, MessageTemplate::kDetachedOperation, method_name);
}

// ES %TypedArray%.prototype.subarray
TF_BUILTIN(TypedArrayPrototypeSubArray, TypedArrayBuiltinsAssembler) {
  const char* method_name = "%TypedArray%.prototype.subarray";
  Label offset_done(this);

  TVARIABLE(Smi, var_begin);
  TVARIABLE(Smi, var_end);

  TNode<Context> context = CAST(Parameter(BuiltinDescriptor::kContext));
  CodeStubArguments args(
      this, ChangeInt32ToIntPtr(Parameter(BuiltinDescriptor::kArgumentsCount)));

  // 1. Let O be the this value.
  // 3. If O does not have a [[TypedArrayName]] internal slot, throw a TypeError
  // exception.
  TNode<Object> receiver = args.GetReceiver();
  ThrowIfNotInstanceType(context, receiver, JS_TYPED_ARRAY_TYPE, method_name);

  TNode<JSTypedArray> source = CAST(receiver);

  // 5. Let buffer be O.[[ViewedArrayBuffer]].
  TNode<JSArrayBuffer> buffer = GetBuffer(context, source);
  // 6. Let srcLength be O.[[ArrayLength]].
  TNode<Smi> source_length =
      LoadObjectField<Smi>(source, JSTypedArray::kLengthOffset);

  // 7. Let relativeBegin be ? ToInteger(begin).
  // 8. If relativeBegin < 0, let beginIndex be max((srcLength + relativeBegin),
  // 0); else let beginIndex be min(relativeBegin, srcLength).
  TNode<Object> begin = args.GetOptionalArgumentValue(0, SmiConstant(0));
  var_begin = ConvertToRelativeIndex(context, begin, source_length);

  TNode<Object> end = args.GetOptionalArgumentValue(1, UndefinedConstant());
  // 9. If end is undefined, let relativeEnd be srcLength;
  var_end = source_length;
  GotoIf(IsUndefined(end), &offset_done);

  // else, let relativeEnd be ? ToInteger(end).
  // 10. If relativeEnd < 0, let endIndex be max((srcLength + relativeEnd), 0);
  // else let endIndex be min(relativeEnd, srcLength).
  var_end = ConvertToRelativeIndex(context, end, source_length);
  Goto(&offset_done);

  BIND(&offset_done);

  // 11. Let newLength be max(endIndex - beginIndex, 0).
  TNode<Smi> new_length = SmiMax(SmiSub(var_end, var_begin), SmiConstant(0));

  // 12. Let constructorName be the String value of O.[[TypedArrayName]].
  // 13. Let elementSize be the Number value of the Element Size value specified
  // in Table 52 for constructorName.
  TNode<Word32T> element_kind = LoadElementsKind(source);
  TNode<IntPtrT> element_size = GetTypedArrayElementSize(element_kind);

  // 14. Let srcByteOffset be O.[[ByteOffset]].
  TNode<Number> source_byte_offset =
      LoadObjectField<Number>(source, JSTypedArray::kByteOffsetOffset);

  // 15. Let beginByteOffset be srcByteOffset + beginIndex × elementSize.
  TNode<Number> offset = SmiMul(var_begin, SmiFromWord(element_size));
  TNode<Number> begin_byte_offset = CAST(NumberAdd(source_byte_offset, offset));

  // 16. Let argumentsList be « buffer, beginByteOffset, newLength ».
  // 17. Return ? TypedArraySpeciesCreate(O, argumentsList).
  args.PopAndReturn(SpeciesCreateByArrayBuffer(
      context, source, buffer, begin_byte_offset, new_length, method_name));
}

// ES #sec-get-%typedarray%.prototype-@@tostringtag
TF_BUILTIN(TypedArrayPrototypeToStringTag, TypedArrayBuiltinsAssembler) {
  Node* receiver = Parameter(Descriptor::kReceiver);
  Label if_receiverisheapobject(this), return_undefined(this);
  Branch(TaggedIsSmi(receiver), &return_undefined, &if_receiverisheapobject);

  // Dispatch on the elements kind, offset by
  // FIRST_FIXED_TYPED_ARRAY_ELEMENTS_KIND.
  size_t const kTypedElementsKindCount = LAST_FIXED_TYPED_ARRAY_ELEMENTS_KIND -
                                         FIRST_FIXED_TYPED_ARRAY_ELEMENTS_KIND +
                                         1;
#define TYPED_ARRAY_CASE(Type, type, TYPE, ctype, size) \
  Label return_##type##array(this);                     \
  BIND(&return_##type##array);                          \
  Return(StringConstant(#Type "Array"));
  TYPED_ARRAYS(TYPED_ARRAY_CASE)
#undef TYPED_ARRAY_CASE
  Label* elements_kind_labels[kTypedElementsKindCount] = {
#define TYPED_ARRAY_CASE(Type, type, TYPE, ctype, size) &return_##type##array,
      TYPED_ARRAYS(TYPED_ARRAY_CASE)
#undef TYPED_ARRAY_CASE
  };
  int32_t elements_kinds[kTypedElementsKindCount] = {
#define TYPED_ARRAY_CASE(Type, type, TYPE, ctype, size) \
  TYPE##_ELEMENTS - FIRST_FIXED_TYPED_ARRAY_ELEMENTS_KIND,
      TYPED_ARRAYS(TYPED_ARRAY_CASE)
#undef TYPED_ARRAY_CASE
  };

  // We offset the dispatch by FIRST_FIXED_TYPED_ARRAY_ELEMENTS_KIND, so
  // that this can be turned into a non-sparse table switch for ideal
  // performance.
  BIND(&if_receiverisheapobject);
  Node* elements_kind =
      Int32Sub(LoadMapElementsKind(LoadMap(receiver)),
               Int32Constant(FIRST_FIXED_TYPED_ARRAY_ELEMENTS_KIND));
  Switch(elements_kind, &return_undefined, elements_kinds, elements_kind_labels,
         kTypedElementsKindCount);

  BIND(&return_undefined);
  Return(UndefinedConstant());
}

void TypedArrayBuiltinsAssembler::GenerateTypedArrayPrototypeIterationMethod(
    Node* context, Node* receiver, const char* method_name,
    IterationKind iteration_kind) {
  Label throw_bad_receiver(this, Label::kDeferred);
  Label throw_typeerror(this, Label::kDeferred);

  GotoIf(TaggedIsSmi(receiver), &throw_bad_receiver);

  Node* map = LoadMap(receiver);
  Node* instance_type = LoadMapInstanceType(map);
  GotoIfNot(InstanceTypeEqual(instance_type, JS_TYPED_ARRAY_TYPE),
            &throw_bad_receiver);

  // Check if the {receiver}'s JSArrayBuffer was neutered.
  Node* receiver_buffer =
      LoadObjectField(receiver, JSTypedArray::kBufferOffset);
  Label if_receiverisneutered(this, Label::kDeferred);
  GotoIf(IsDetachedBuffer(receiver_buffer), &if_receiverisneutered);

  Return(CreateArrayIterator(receiver, map, instance_type, context,
                             iteration_kind));

  VARIABLE(var_message, MachineRepresentation::kTagged);
  BIND(&throw_bad_receiver);
  var_message.Bind(SmiConstant(MessageTemplate::kNotTypedArray));
  Goto(&throw_typeerror);

  BIND(&if_receiverisneutered);
  var_message.Bind(SmiConstant(MessageTemplate::kDetachedOperation));
  Goto(&throw_typeerror);

  BIND(&throw_typeerror);
  {
    Node* method_arg = StringConstant(method_name);
    Node* result = CallRuntime(Runtime::kThrowTypeError, context,
                               var_message.value(), method_arg);
    Return(result);
  }
}

// ES6 #sec-%typedarray%.prototype.values
TF_BUILTIN(TypedArrayPrototypeValues, TypedArrayBuiltinsAssembler) {
  Node* context = Parameter(Descriptor::kContext);
  Node* receiver = Parameter(Descriptor::kReceiver);
  GenerateTypedArrayPrototypeIterationMethod(context, receiver,
                                             "%TypedArray%.prototype.values()",
                                             IterationKind::kValues);
}

// ES6 #sec-%typedarray%.prototype.entries
TF_BUILTIN(TypedArrayPrototypeEntries, TypedArrayBuiltinsAssembler) {
  Node* context = Parameter(Descriptor::kContext);
  Node* receiver = Parameter(Descriptor::kReceiver);
  GenerateTypedArrayPrototypeIterationMethod(context, receiver,
                                             "%TypedArray%.prototype.entries()",
                                             IterationKind::kEntries);
}

// ES6 #sec-%typedarray%.prototype.keys
TF_BUILTIN(TypedArrayPrototypeKeys, TypedArrayBuiltinsAssembler) {
  Node* context = Parameter(Descriptor::kContext);
  Node* receiver = Parameter(Descriptor::kReceiver);
  GenerateTypedArrayPrototypeIterationMethod(
      context, receiver, "%TypedArray%.prototype.keys()", IterationKind::kKeys);
}

#undef V8_TYPED_ARRAY_MAX_SIZE_IN_HEAP

}  // namespace internal
}  // namespace v8
