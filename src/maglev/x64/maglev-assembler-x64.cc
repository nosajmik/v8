// Copyright 2022 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/codegen/interface-descriptors-inl.h"
#include "src/common/globals.h"
#include "src/maglev/maglev-graph.h"
#include "src/maglev/x64/maglev-assembler-x64-inl.h"
#include "src/objects/heap-number.h"

namespace v8 {
namespace internal {
namespace maglev {

#define __ masm->

void MaglevAssembler::Allocate(RegisterSnapshot& register_snapshot,
                               Register object, int size_in_bytes,
                               AllocationType alloc_type,
                               AllocationAlignment alignment) {
  // TODO(victorgomes): Call the runtime for large object allocation.
  // TODO(victorgomes): Support double alignment.
  DCHECK_EQ(alignment, kTaggedAligned);
  size_in_bytes = ALIGN_TO_ALLOCATION_ALIGNMENT(size_in_bytes);
  if (v8_flags.single_generation) {
    alloc_type = AllocationType::kOld;
  }
  bool in_new_space = alloc_type == AllocationType::kYoung;
  ExternalReference top =
      in_new_space
          ? ExternalReference::new_space_allocation_top_address(isolate_)
          : ExternalReference::old_space_allocation_top_address(isolate_);
  ExternalReference limit =
      in_new_space
          ? ExternalReference::new_space_allocation_limit_address(isolate_)
          : ExternalReference::old_space_allocation_limit_address(isolate_);

  ZoneLabelRef done(this);
  Register new_top = kScratchRegister;
  // Check if there is enough space.
  Move(object, ExternalReferenceAsOperand(top));
  leaq(new_top, Operand(object, size_in_bytes));
  cmpq(new_top, ExternalReferenceAsOperand(limit));
  // Otherwise call runtime.
  JumpToDeferredIf(
      greater_equal,
      [](MaglevAssembler* masm, RegisterSnapshot register_snapshot,
         Register object, Builtin builtin, int size_in_bytes,
         ZoneLabelRef done) {
        // Remove {object} from snapshot, since it is the returned allocated
        // HeapObject.
        register_snapshot.live_registers.clear(object);
        register_snapshot.live_tagged_registers.clear(object);
        {
          SaveRegisterStateForCall save_register_state(masm, register_snapshot);
          using D = AllocateDescriptor;
          __ Move(D::GetRegisterParameter(D::kRequestedSize), size_in_bytes);
          __ CallBuiltin(builtin);
          save_register_state.DefineSafepoint();
          __ Move(object, kReturnRegister0);
        }
        __ jmp(*done);
      },
      register_snapshot, object,
      in_new_space ? Builtin::kAllocateRegularInYoungGeneration
                   : Builtin::kAllocateRegularInOldGeneration,
      size_in_bytes, done);
  // Store new top and tag object.
  movq(ExternalReferenceAsOperand(top), new_top);
  addq(object, Immediate(kHeapObjectTag));
  bind(*done);
}

void MaglevAssembler::AllocateHeapNumber(RegisterSnapshot register_snapshot,
                                         Register result,
                                         DoubleRegister value) {
  // In the case we need to call the runtime, we should spill the value
  // register. Even if it is not live in the next node, otherwise the
  // allocation call might trash it.
  register_snapshot.live_double_registers.set(value);
  Allocate(register_snapshot, result, HeapNumber::kSize);
  LoadRoot(kScratchRegister, RootIndex::kHeapNumberMap);
  StoreTaggedField(FieldOperand(result, HeapObject::kMapOffset),
                   kScratchRegister);
  Movsd(FieldOperand(result, HeapNumber::kValueOffset), value);
}

void MaglevAssembler::AllocateTwoByteString(RegisterSnapshot register_snapshot,
                                            Register result, int length) {
  int size = SeqTwoByteString::SizeFor(length);
  Allocate(register_snapshot, result, size);
  LoadRoot(kScratchRegister, RootIndex::kStringMap);
  StoreTaggedField(FieldOperand(result, size - kObjectAlignment), Immediate(0));
  StoreTaggedField(FieldOperand(result, HeapObject::kMapOffset),
                   kScratchRegister);
  StoreTaggedField(FieldOperand(result, Name::kRawHashFieldOffset),
                   Immediate(Name::kEmptyHashField));
  StoreTaggedField(FieldOperand(result, String::kLengthOffset),
                   Immediate(length));
}

void MaglevAssembler::LoadSingleCharacterString(Register result,
                                                Register char_code,
                                                Register scratch) {
  AssertZeroExtended(char_code);
  if (v8_flags.debug_code) {
    cmpq(char_code, Immediate(String::kMaxOneByteCharCode));
    Assert(below_equal, AbortReason::kUnexpectedValue);
  }
  DCHECK_NE(char_code, scratch);
  Register table = scratch;
  LoadRoot(table, RootIndex::kSingleCharacterStringTable);
  DecompressAnyTagged(result, FieldOperand(table, char_code, times_tagged_size,
                                           FixedArray::kHeaderSize));
}

void MaglevAssembler::StringFromCharCode(RegisterSnapshot register_snapshot,
                                         Label* char_code_fits_one_byte,
                                         Register result, Register char_code,
                                         Register scratch) {
  DCHECK_NE(char_code, scratch);
  ZoneLabelRef done(this);
  cmpl(char_code, Immediate(String::kMaxOneByteCharCode));
  JumpToDeferredIf(
      above,
      [](MaglevAssembler* masm, RegisterSnapshot register_snapshot,
         ZoneLabelRef done, Register result, Register char_code,
         Register scratch) {
        // Be sure to save {char_code}. If it aliases with {result}, use
        // the scratch register.
        if (char_code == result) {
          // This is guaranteed to be true since we've already checked
          // char_code != scratch.
          DCHECK_NE(scratch, result);
          __ Move(scratch, char_code);
          char_code = scratch;
        }
        DCHECK(!register_snapshot.live_tagged_registers.has(char_code));
        register_snapshot.live_registers.set(char_code);
        __ AllocateTwoByteString(register_snapshot, result, 1);
        __ andl(char_code, Immediate(0xFFFF));
        __ movw(FieldOperand(result, SeqTwoByteString::kHeaderSize), char_code);
        __ jmp(*done);
      },
      register_snapshot, done, result, char_code, scratch);
  if (char_code_fits_one_byte != nullptr) {
    bind(char_code_fits_one_byte);
  }
  LoadSingleCharacterString(result, char_code, scratch);
  bind(*done);
}

void MaglevAssembler::StringCharCodeAt(RegisterSnapshot& register_snapshot,
                                       Register result, Register string,
                                       Register index, Register scratch,
                                       Label* result_fits_one_byte) {
  ZoneLabelRef done(this);
  Label seq_string;
  Label cons_string;
  Label sliced_string;

  DeferredCodeInfo* deferred_runtime_call = PushDeferredCode(
      [](MaglevAssembler* masm, RegisterSnapshot register_snapshot,
         ZoneLabelRef done, Register result, Register string, Register index) {
        DCHECK(!register_snapshot.live_registers.has(result));
        DCHECK(!register_snapshot.live_registers.has(string));
        DCHECK(!register_snapshot.live_registers.has(index));
        {
          SaveRegisterStateForCall save_register_state(masm, register_snapshot);
          __ Push(string);
          __ SmiTag(index);
          __ Push(index);
          __ Move(kContextRegister, masm->native_context().object());
          // This call does not throw nor can deopt.
          __ CallRuntime(Runtime::kStringCharCodeAt);
          save_register_state.DefineSafepoint();
          __ SmiUntag(kReturnRegister0);
          __ Move(result, kReturnRegister0);
        }
        __ jmp(*done);
      },
      register_snapshot, done, result, string, index);

  Register instance_type = scratch;

  // We might need to try more than one time for ConsString, SlicedString and
  // ThinString.
  Label loop;
  bind(&loop);

  if (v8_flags.debug_code) {
    // Check if {string} is a string.
    AssertNotSmi(string);
    LoadMap(scratch, string);
    CmpInstanceTypeRange(scratch, scratch, FIRST_STRING_TYPE, LAST_STRING_TYPE);
    Check(below_equal, AbortReason::kUnexpectedValue);

    movl(scratch, FieldOperand(string, String::kLengthOffset));
    cmpl(index, scratch);
    Check(below, AbortReason::kUnexpectedValue);
  }

  // Get instance type.
  LoadMap(instance_type, string);
  mov_tagged(instance_type,
             FieldOperand(instance_type, Map::kInstanceTypeOffset));

  {
    // TODO(victorgomes): Add fast path for external strings.
    Register representation = kScratchRegister;
    movl(representation, instance_type);
    andl(representation, Immediate(kStringRepresentationMask));
    cmpl(representation, Immediate(kSeqStringTag));
    j(equal, &seq_string, Label::kNear);
    cmpl(representation, Immediate(kConsStringTag));
    j(equal, &cons_string, Label::kNear);
    cmpl(representation, Immediate(kSlicedStringTag));
    j(equal, &sliced_string, Label::kNear);
    cmpl(representation, Immediate(kThinStringTag));
    j(not_equal, &deferred_runtime_call->deferred_code_label);
    // Fallthrough to thin string.
  }

  // Is a thin string.
  {
    DecompressAnyTagged(string,
                        FieldOperand(string, ThinString::kActualOffset));
    jmp(&loop, Label::kNear);
  }

  bind(&sliced_string);
  {
    Register offset = scratch;
    movl(offset, FieldOperand(string, SlicedString::kOffsetOffset));
    SmiUntag(offset);
    DecompressAnyTagged(string,
                        FieldOperand(string, SlicedString::kParentOffset));
    addl(index, offset);
    jmp(&loop, Label::kNear);
  }

  bind(&cons_string);
  {
    CompareRoot(FieldOperand(string, ConsString::kSecondOffset),
                RootIndex::kempty_string);
    j(not_equal, &deferred_runtime_call->deferred_code_label);
    DecompressAnyTagged(string, FieldOperand(string, ConsString::kFirstOffset));
    jmp(&loop, Label::kNear);  // Try again with first string.
  }

  bind(&seq_string);
  {
    Label two_byte_string;
    andl(instance_type, Immediate(kStringEncodingMask));
    cmpl(instance_type, Immediate(kTwoByteStringTag));
    j(equal, &two_byte_string, Label::kNear);
    movzxbl(result, FieldOperand(string, index, times_1,
                                 SeqOneByteString::kHeaderSize));
    jmp(result_fits_one_byte);
    bind(&two_byte_string);
    movzxwl(result, FieldOperand(string, index, times_2,
                                 SeqTwoByteString::kHeaderSize));
    // Fallthrough.
  }

  bind(*done);

  if (v8_flags.debug_code) {
    // We make sure that the user of this macro is not relying in string and
    // index to not be clobbered.
    if (result != string) {
      movl(string, Immediate(0xdeadbeef));
    }
    if (result != index) {
      movl(index, Immediate(0xdeadbeef));
    }
  }
}

void MaglevAssembler::ToBoolean(Register value, ZoneLabelRef is_true,
                                ZoneLabelRef is_false,
                                bool fallthrough_when_true) {
  Register map = kScratchRegister;

  // Check if {{value}} is Smi.
  CheckSmi(value);
  JumpToDeferredIf(
      zero,
      [](MaglevAssembler* masm, Register value, ZoneLabelRef is_true,
         ZoneLabelRef is_false) {
        // Check if {value} is not zero.
        __ SmiCompare(value, Smi::FromInt(0));
        __ j(equal, *is_false);
        __ jmp(*is_true);
      },
      value, is_true, is_false);

  // Check if {{value}} is false.
  CompareRoot(value, RootIndex::kFalseValue);
  j(equal, *is_false);

  // Check if {{value}} is empty string.
  CompareRoot(value, RootIndex::kempty_string);
  j(equal, *is_false);

  // Check if {{value}} is undetectable.
  LoadMap(map, value);
  testl(FieldOperand(map, Map::kBitFieldOffset),
        Immediate(Map::Bits1::IsUndetectableBit::kMask));
  j(not_zero, *is_false);

  // Check if {{value}} is a HeapNumber.
  CompareRoot(map, RootIndex::kHeapNumberMap);
  JumpToDeferredIf(
      equal,
      [](MaglevAssembler* masm, Register value, ZoneLabelRef is_true,
         ZoneLabelRef is_false) {
        // Sets scratch register to 0.0.
        __ Xorpd(kScratchDoubleReg, kScratchDoubleReg);
        // Sets ZF if equal to 0.0, -0.0 or NaN.
        __ Ucomisd(kScratchDoubleReg,
                   FieldOperand(value, HeapNumber::kValueOffset));
        __ j(zero, *is_false);
        __ jmp(*is_true);
      },
      value, is_true, is_false);

  // Check if {{value}} is a BigInt.
  CompareRoot(map, RootIndex::kBigIntMap);
  JumpToDeferredIf(
      equal,
      [](MaglevAssembler* masm, Register value, ZoneLabelRef is_true,
         ZoneLabelRef is_false) {
        __ testl(FieldOperand(value, BigInt::kBitfieldOffset),
                 Immediate(BigInt::LengthBits::kMask));
        __ j(zero, *is_false);
        __ jmp(*is_true);
      },
      value, is_true, is_false);

  // Otherwise true.
  if (!fallthrough_when_true) {
    jmp(*is_true);
  }
}

void MaglevAssembler::TruncateDoubleToInt32(Register dst, DoubleRegister src) {
  ZoneLabelRef done(this);

  Cvttsd2siq(dst, src);
  // Check whether the Cvt overflowed.
  cmpq(dst, Immediate(1));
  JumpToDeferredIf(
      overflow,
      [](MaglevAssembler* masm, DoubleRegister src, Register dst,
         ZoneLabelRef done) {
        // Push the double register onto the stack as an input argument.
        __ AllocateStackSpace(kDoubleSize);
        __ Movsd(MemOperand(rsp, 0), src);
        __ CallBuiltin(Builtin::kDoubleToI);
        // DoubleToI sets the result on the stack, pop the result off the stack.
        // Avoid using `pop` to not mix implicit and explicit rsp updates.
        __ movl(dst, MemOperand(rsp, 0));
        __ addq(rsp, Immediate(kDoubleSize));
        __ jmp(*done);
      },
      src, dst, done);
  bind(*done);
  // Zero extend the converted value to complete the truncation.
  movl(dst, dst);
}

void MaglevAssembler::Prologue(Graph* graph) {
  if (v8_flags.maglev_ool_prologue) {
    // Call the out-of-line prologue (with parameters passed on the stack).
    Push(Immediate(code_gen_state()->stack_slots() * kSystemPointerSize));
    Push(Immediate(code_gen_state()->tagged_slots() * kSystemPointerSize));
    CallBuiltin(Builtin::kMaglevOutOfLinePrologue);
    return;
  }

  BailoutIfDeoptimized(rbx);

  // Tiering support.
  // TODO(jgruber): Extract to a builtin (the tiering prologue is ~230 bytes
  // per Maglev code object on x64).
  {
    // Scratch registers. Don't clobber regs related to the calling
    // convention (e.g. kJavaScriptCallArgCountRegister). Keep up-to-date
    // with deferred flags code.
    Register flags = rcx;
    Register feedback_vector = r9;

    // Load the feedback vector.
    LoadTaggedPointerField(
        feedback_vector,
        FieldOperand(kJSFunctionRegister, JSFunction::kFeedbackCellOffset));
    LoadTaggedPointerField(feedback_vector,
                           FieldOperand(feedback_vector, Cell::kValueOffset));
    AssertFeedbackVector(feedback_vector);

    DeferredCodeInfo* deferred_flags_need_processing = PushDeferredCode(
        [](MaglevAssembler* masm, Register flags, Register feedback_vector) {
          ASM_CODE_COMMENT_STRING(masm, "Optimized marker check");
          // TODO(leszeks): This could definitely be a builtin that we
          // tail-call.
          __ OptimizeCodeOrTailCallOptimizedCodeSlot(
              flags, feedback_vector, kJSFunctionRegister, JumpMode::kJump);
          __ Trap();
        },
        flags, feedback_vector);

    LoadFeedbackVectorFlagsAndJumpIfNeedsProcessing(
        flags, feedback_vector, CodeKind::MAGLEV,
        &deferred_flags_need_processing->deferred_code_label);
  }

  EnterFrame(StackFrame::MAGLEV);

  // Save arguments in frame.
  // TODO(leszeks): Consider eliding this frame if we don't make any calls
  // that could clobber these registers.
  Push(kContextRegister);
  Push(kJSFunctionRegister);              // Callee's JS function.
  Push(kJavaScriptCallArgCountRegister);  // Actual argument count.

  {
    ASM_CODE_COMMENT_STRING(this, " Stack/interrupt check");
    // Stack check. This folds the checks for both the interrupt stack limit
    // check and the real stack limit into one by just checking for the
    // interrupt limit. The interrupt limit is either equal to the real
    // stack limit or tighter. By ensuring we have space until that limit
    // after building the frame we can quickly precheck both at once.
    Move(kScratchRegister, rsp);
    const int max_stack_slots_used =
        code_gen_state()->stack_slots() + graph->max_call_stack_args();
    const int max_stack_size =
        std::max(static_cast<int>(graph->max_deopted_stack_size()),
                 max_stack_slots_used * kSystemPointerSize);
    subq(kScratchRegister, Immediate(max_stack_size));
    cmpq(kScratchRegister,
         StackLimitAsOperand(StackLimitKind::kInterruptStackLimit));

    ZoneLabelRef deferred_call_stack_guard_return(this);
    JumpToDeferredIf(
        below_equal,
        [](MaglevAssembler* masm, ZoneLabelRef done, int max_stack_size) {
          ASM_CODE_COMMENT_STRING(masm, "Stack/interrupt call");
          // Save any registers that can be referenced by RegisterInput.
          // TODO(leszeks): Only push those that are used by the graph.
          __ PushAll(RegisterInput::kAllowedRegisters);
          // Push the frame size
          __ Push(Immediate(Smi::FromInt(max_stack_size)));
          __ CallRuntime(Runtime::kStackGuardWithGap, 1);
          __ PopAll(RegisterInput::kAllowedRegisters);
          __ jmp(*done);
        },
        deferred_call_stack_guard_return, max_stack_size);
    bind(*deferred_call_stack_guard_return);
  }

  // Initialize stack slots.
  if (graph->tagged_stack_slots() > 0) {
    ASM_CODE_COMMENT_STRING(this, "Initializing stack slots");
    // TODO(leszeks): Consider filling with xmm + movdqa instead.
    Move(rax, 0);

    // Magic value. Experimentally, an unroll size of 8 doesn't seem any
    // worse than fully unrolled pushes.
    const int kLoopUnrollSize = 8;
    int tagged_slots = graph->tagged_stack_slots();
    if (tagged_slots < 2 * kLoopUnrollSize) {
      // If the frame is small enough, just unroll the frame fill
      // completely.
      for (int i = 0; i < tagged_slots; ++i) {
        pushq(rax);
      }
    } else {
      // Extract the first few slots to round to the unroll size.
      int first_slots = tagged_slots % kLoopUnrollSize;
      for (int i = 0; i < first_slots; ++i) {
        pushq(rax);
      }
      Move(rbx, tagged_slots / kLoopUnrollSize);
      // We enter the loop unconditionally, so make sure we need to loop at
      // least once.
      DCHECK_GT(tagged_slots / kLoopUnrollSize, 0);
      Label loop;
      bind(&loop);
      for (int i = 0; i < kLoopUnrollSize; ++i) {
        pushq(rax);
      }
      decl(rbx);
      j(greater, &loop);
    }
  }
  if (graph->untagged_stack_slots() > 0) {
    // Extend rsp by the size of the remaining untagged part of the frame,
    // no need to initialise these.
    subq(rsp, Immediate(graph->untagged_stack_slots() * kSystemPointerSize));
  }
}

void MaglevAssembler::MaybeEmitDeoptBuiltinsCall(size_t eager_deopt_count,
                                                 Label* eager_deopt_entry,
                                                 size_t lazy_deopt_count,
                                                 Label* lazy_deopt_entry) {}

}  // namespace maglev
}  // namespace internal
}  // namespace v8
