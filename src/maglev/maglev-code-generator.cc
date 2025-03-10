// Copyright 2022 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/maglev/maglev-code-generator.h"

#include <algorithm>

#include "src/base/hashmap.h"
#include "src/codegen/code-desc.h"
#include "src/codegen/interface-descriptors-inl.h"
#include "src/codegen/interface-descriptors.h"
#include "src/codegen/register.h"
#include "src/codegen/reglist.h"
#include "src/codegen/safepoint-table.h"
#include "src/codegen/source-position.h"
#include "src/common/globals.h"
#include "src/compiler/backend/instruction.h"
#include "src/deoptimizer/deoptimize-reason.h"
#include "src/deoptimizer/translation-array.h"
#include "src/execution/frame-constants.h"
#include "src/interpreter/bytecode-register.h"
#include "src/maglev/maglev-code-gen-state.h"
#include "src/maglev/maglev-compilation-unit.h"
#include "src/maglev/maglev-graph-labeller.h"
#include "src/maglev/maglev-graph-printer.h"
#include "src/maglev/maglev-graph-processor.h"
#include "src/maglev/maglev-graph.h"
#include "src/maglev/maglev-ir.h"
#include "src/maglev/maglev-regalloc-data.h"
#include "src/objects/code-inl.h"
#include "src/utils/identity-map.h"

#ifdef V8_TARGET_ARCH_ARM64
#include "src/maglev/arm64/maglev-assembler-arm64-inl.h"
#elif V8_TARGET_ARCH_X64
#include "src/maglev/x64/maglev-assembler-x64-inl.h"
#else
#error "Maglev does not supported this architecture."
#endif

namespace v8 {
namespace internal {
namespace maglev {

#define __ masm()->

namespace {

template <typename RegisterT>
struct RegisterTHelper;
template <>
struct RegisterTHelper<Register> {
  static constexpr Register kScratch = kScratchRegister;
  static constexpr RegList kAllocatableRegisters = kAllocatableGeneralRegisters;
};
template <>
struct RegisterTHelper<DoubleRegister> {
  static constexpr DoubleRegister kScratch = kScratchDoubleReg;
  static constexpr DoubleRegList kAllocatableRegisters =
      kAllocatableDoubleRegisters;
};

// The ParallelMoveResolver is used to resolve multiple moves between registers
// and stack slots that are intended to happen, semantically, in parallel. It
// finds chains of moves that would clobber each other, and emits them in a non
// clobbering order; it also detects cycles of moves and breaks them by moving
// to a temporary.
//
// For example, given the moves:
//
//     r1 -> r2
//     r2 -> r3
//     r3 -> r4
//     r4 -> r1
//     r4 -> r5
//
// These can be represented as a move graph
//
//     r2 → r3
//     ↑     ↓
//     r1 ← r4 → r5
//
// and safely emitted (breaking the cycle with a temporary) as
//
//     r1 -> tmp
//     r4 -> r1
//     r4 -> r5
//     r3 -> r4
//     r2 -> r3
//    tmp -> r2
//
// It additionally keeps track of materialising moves, which don't have a stack
// slot but rather materialise a value from, e.g., a constant. These can safely
// be emitted at the end, once all the parallel moves are done.
template <typename RegisterT>
class ParallelMoveResolver {
  static constexpr RegisterT kScratchRegT =
      RegisterTHelper<RegisterT>::kScratch;

  static constexpr auto kAllocatableRegistersT =
      RegisterTHelper<RegisterT>::kAllocatableRegisters;

 public:
  explicit ParallelMoveResolver(MaglevAssembler* masm) : masm_(masm) {}

  void RecordMove(ValueNode* source_node, compiler::InstructionOperand source,
                  compiler::AllocatedOperand target) {
    if (target.IsRegister()) {
      RecordMoveToRegister(source_node, source, ToRegisterT<RegisterT>(target));
    } else {
      RecordMoveToStackSlot(source_node, source,
                            masm_->GetFramePointerOffsetForStackSlot(target));
    }
  }

  void RecordMove(ValueNode* source_node, compiler::InstructionOperand source,
                  RegisterT target_reg) {
    RecordMoveToRegister(source_node, source, target_reg);
  }

  void EmitMoves() {
    for (RegisterT reg : kAllocatableRegistersT) {
      StartEmitMoveChain(reg);
      ValueNode* materializing_register_move =
          materializing_register_moves_[reg.code()];
      if (materializing_register_move) {
        materializing_register_move->LoadToRegister(masm_, reg);
      }
    }
    // Emit stack moves until the move set is empty -- each EmitMoveChain will
    // pop entries off the moves_from_stack_slot map so we can't use a simple
    // iteration here.
    while (!moves_from_stack_slot_.empty()) {
      StartEmitMoveChain(moves_from_stack_slot_.begin()->first);
    }
    for (auto [stack_slot, node] : materializing_stack_slot_moves_) {
      node->LoadToRegister(masm_, kScratchRegT);
      __ Move(StackSlot{stack_slot}, kScratchRegT);
    }
  }

  ParallelMoveResolver(ParallelMoveResolver&&) = delete;
  ParallelMoveResolver operator=(ParallelMoveResolver&&) = delete;
  ParallelMoveResolver(const ParallelMoveResolver&) = delete;
  ParallelMoveResolver operator=(const ParallelMoveResolver&) = delete;

 private:
  // The targets of moves from a source, i.e. the set of outgoing edges for a
  // node in the move graph.
  struct GapMoveTargets {
    RegListBase<RegisterT> registers;
    base::SmallVector<int32_t, 1> stack_slots = base::SmallVector<int32_t, 1>{};

    GapMoveTargets() = default;
    GapMoveTargets(GapMoveTargets&&) V8_NOEXCEPT = default;
    GapMoveTargets& operator=(GapMoveTargets&&) V8_NOEXCEPT = default;
    GapMoveTargets(const GapMoveTargets&) = delete;
    GapMoveTargets& operator=(const GapMoveTargets&) = delete;

    bool is_empty() const {
      return registers.is_empty() && stack_slots.empty();
    }
  };

#ifdef DEBUG
  void CheckNoExistingMoveToRegister(RegisterT target_reg) {
    for (RegisterT reg : kAllocatableRegistersT) {
      if (moves_from_register_[reg.code()].registers.has(target_reg)) {
        FATAL("Existing move from %s to %s", RegisterName(reg),
              RegisterName(target_reg));
      }
    }
    for (auto& [stack_slot, targets] : moves_from_stack_slot_) {
      if (targets.registers.has(target_reg)) {
        FATAL("Existing move from stack slot %d to %s", stack_slot,
              RegisterName(target_reg));
      }
    }
    if (materializing_register_moves_[target_reg.code()] != nullptr) {
      FATAL("Existing materialization of %p to %s",
            materializing_register_moves_[target_reg.code()],
            RegisterName(target_reg));
    }
  }

  void CheckNoExistingMoveToStackSlot(int32_t target_slot) {
    for (Register reg : kAllocatableRegistersT) {
      auto& stack_slots = moves_from_register_[reg.code()].stack_slots;
      if (std::any_of(stack_slots.begin(), stack_slots.end(),
                      [&](int32_t slot) { return slot == target_slot; })) {
        FATAL("Existing move from %s to stack slot %d", RegisterName(reg),
              target_slot);
      }
    }
    for (auto& [stack_slot, targets] : moves_from_stack_slot_) {
      auto& stack_slots = targets.stack_slots;
      if (std::any_of(stack_slots.begin(), stack_slots.end(),
                      [&](int32_t slot) { return slot == target_slot; })) {
        FATAL("Existing move from stack slot %d to stack slot %d", stack_slot,
              target_slot);
      }
    }
    for (auto& [stack_slot, node] : materializing_stack_slot_moves_) {
      if (stack_slot == target_slot) {
        FATAL("Existing materialization of %p to stack slot %d", node,
              stack_slot);
      }
    }
  }
#else
  void CheckNoExistingMoveToRegister(RegisterT target_reg) {}
  void CheckNoExistingMoveToStackSlot(int32_t target_slot) {}
#endif

  void RecordMoveToRegister(ValueNode* node,
                            compiler::InstructionOperand source,
                            RegisterT target_reg) {
    // There shouldn't have been another move to this register already.
    CheckNoExistingMoveToRegister(target_reg);

    if (source.IsAnyRegister()) {
      RegisterT source_reg = ToRegisterT<RegisterT>(source);
      if (target_reg != source_reg) {
        moves_from_register_[source_reg.code()].registers.set(target_reg);
      }
    } else if (source.IsAnyStackSlot()) {
      int32_t source_slot = masm_->GetFramePointerOffsetForStackSlot(
          compiler::AllocatedOperand::cast(source));
      moves_from_stack_slot_[source_slot].registers.set(target_reg);
    } else {
      DCHECK(source.IsConstant());
      DCHECK(IsConstantNode(node->opcode()));
      materializing_register_moves_[target_reg.code()] = node;
    }
  }

  void RecordMoveToStackSlot(ValueNode* node,
                             compiler::InstructionOperand source,
                             int32_t target_slot) {
    // There shouldn't have been another move to this stack slot already.
    CheckNoExistingMoveToStackSlot(target_slot);

    if (source.IsAnyRegister()) {
      RegisterT source_reg = ToRegisterT<RegisterT>(source);
      moves_from_register_[source_reg.code()].stack_slots.push_back(
          target_slot);
    } else if (source.IsAnyStackSlot()) {
      int32_t source_slot = masm_->GetFramePointerOffsetForStackSlot(
          compiler::AllocatedOperand::cast(source));
      if (source_slot != target_slot) {
        moves_from_stack_slot_[source_slot].stack_slots.push_back(target_slot);
      }
    } else {
      DCHECK(source.IsConstant());
      DCHECK(IsConstantNode(node->opcode()));
      materializing_stack_slot_moves_.emplace_back(target_slot, node);
    }
  }

  // Finds and clears the targets for a given source. In terms of move graph,
  // this returns and removes all outgoing edges from the source.
  GapMoveTargets PopTargets(RegisterT source_reg) {
    return std::exchange(moves_from_register_[source_reg.code()],
                         GapMoveTargets{});
  }
  GapMoveTargets PopTargets(int32_t source_slot) {
    auto handle = moves_from_stack_slot_.extract(source_slot);
    if (handle.empty()) return {};
    DCHECK(!handle.mapped().is_empty());
    return std::move(handle.mapped());
  }

  // Emit a single move chain starting at the given source (either a register or
  // a stack slot). This is a destructive operation on the move graph, and
  // removes the emitted edges from the graph. Subsequent calls with the same
  // source should emit no code.
  template <typename SourceT>
  void StartEmitMoveChain(SourceT source) {
    DCHECK(!scratch_has_cycle_start_);
    GapMoveTargets targets = PopTargets(source);
    if (targets.is_empty()) return;

    // Start recursively emitting the move chain, with this source as the start
    // of the chain.
    bool has_cycle = RecursivelyEmitMoveChainTargets(source, targets);

    // Each connected component in the move graph can only have one cycle
    // (proof: each target can only have one incoming edge, so cycles in the
    // graph can only have outgoing edges, so there's no way to connect two
    // cycles). This means that if there's a cycle, the saved value must be the
    // chain start.
    if (has_cycle) {
      if (!scratch_has_cycle_start_) {
        Pop(kScratchRegT);
        scratch_has_cycle_start_ = true;
      }
      EmitMovesFromSource(kScratchRegT, std::move(targets));
      scratch_has_cycle_start_ = false;
      __ RecordComment("--   * End of cycle");
    } else {
      EmitMovesFromSource(source, std::move(targets));
      __ RecordComment("--   * Chain emitted with no cycles");
    }
  }

  template <typename ChainStartT, typename SourceT>
  bool ContinueEmitMoveChain(ChainStartT chain_start, SourceT source) {
    if constexpr (std::is_same_v<ChainStartT, SourceT>) {
      // If the recursion has returned to the start of the chain, then this must
      // be a cycle.
      if (chain_start == source) {
        __ RecordComment("--   * Cycle");
        DCHECK(!scratch_has_cycle_start_);
        if constexpr (std::is_same_v<ChainStartT, int32_t>) {
          __ Move(kScratchRegT, StackSlot{chain_start});
        } else {
          __ Move(kScratchRegT, chain_start);
        }
        scratch_has_cycle_start_ = true;
        return true;
      }
    }

    GapMoveTargets targets = PopTargets(source);
    if (targets.is_empty()) {
      __ RecordComment("--   * End of chain");
      return false;
    }

    bool has_cycle = RecursivelyEmitMoveChainTargets(chain_start, targets);

    EmitMovesFromSource(source, std::move(targets));
    return has_cycle;
  }

  // Calls RecursivelyEmitMoveChain for each target of a source. This is used to
  // share target visiting code between StartEmitMoveChain and
  // ContinueEmitMoveChain.
  template <typename ChainStartT>
  bool RecursivelyEmitMoveChainTargets(ChainStartT chain_start,
                                       GapMoveTargets& targets) {
    bool has_cycle = false;
    for (auto target : targets.registers) {
      has_cycle |= ContinueEmitMoveChain(chain_start, target);
    }
    for (int32_t target_slot : targets.stack_slots) {
      has_cycle |= ContinueEmitMoveChain(chain_start, target_slot);
    }
    return has_cycle;
  }

  void EmitMovesFromSource(RegisterT source_reg, GapMoveTargets&& targets) {
    DCHECK(moves_from_register_[source_reg.code()].is_empty());
    for (RegisterT target_reg : targets.registers) {
      DCHECK(moves_from_register_[target_reg.code()].is_empty());
      __ Move(target_reg, source_reg);
    }
    for (int32_t target_slot : targets.stack_slots) {
      DCHECK_EQ(moves_from_stack_slot_.find(target_slot),
                moves_from_stack_slot_.end());
      __ Move(StackSlot{target_slot}, source_reg);
    }
  }

  void EmitMovesFromSource(int32_t source_slot, GapMoveTargets&& targets) {
    DCHECK_EQ(moves_from_stack_slot_.find(source_slot),
              moves_from_stack_slot_.end());

    // Cache the slot value on a register.
    RegisterT register_with_slot_value = RegisterT::no_reg();
    if (!targets.registers.is_empty()) {
      // If one of the targets is a register, we can move our value into it and
      // optimize the moves from this stack slot to always be via that register.
      register_with_slot_value = targets.registers.PopFirst();
    } else {
      DCHECK(!targets.stack_slots.empty());
      // Otherwise, cache the slot value on the scratch register, clobbering it
      // if necessary.
      if (scratch_has_cycle_start_) {
        Push(kScratchRegT);
        scratch_has_cycle_start_ = false;
      }
      register_with_slot_value = kScratchRegT;
    }

    // Now emit moves from that cached register instead of from the stack slot.
    DCHECK(register_with_slot_value.is_valid());
    DCHECK(moves_from_register_[register_with_slot_value.code()].is_empty());
    __ Move(register_with_slot_value, StackSlot{source_slot});
    EmitMovesFromSource(register_with_slot_value, std::move(targets));
  }

  void Push(Register reg) { __ Push(reg); }
  void Push(DoubleRegister reg) { __ PushAll({reg}); }
  void Pop(Register reg) { __ Pop(reg); }
  void Pop(DoubleRegister reg) { __ PopAll({reg}); }

  MaglevAssembler* masm() const { return masm_; }

  MaglevAssembler* const masm_;

  // Keep moves to/from registers and stack slots separate -- there are a fixed
  // number of registers but an infinite number of stack slots, so the register
  // moves can be kept in a fixed size array while the stack slot moves need a
  // map.

  // moves_from_register_[source] = target.
  std::array<GapMoveTargets, RegisterT::kNumRegisters> moves_from_register_ =
      {};

  // TODO(victorgomes): Use MaglevAssembler::StackSlot instead of int32_t.
  // moves_from_stack_slot_[source] = target.
  std::unordered_map<int32_t, GapMoveTargets> moves_from_stack_slot_;

  // materializing_register_moves[target] = node.
  std::array<ValueNode*, RegisterT::kNumRegisters>
      materializing_register_moves_ = {};

  // materializing_stack_slot_moves = {(node,target), ... }.
  std::vector<std::pair<int32_t, ValueNode*>> materializing_stack_slot_moves_;

  bool scratch_has_cycle_start_ = false;
};

class ExceptionHandlerTrampolineBuilder {
 public:
  static void Build(MaglevAssembler* masm, NodeBase* node) {
    ExceptionHandlerTrampolineBuilder builder(masm);
    builder.EmitTrampolineFor(node);
  }

 private:
  explicit ExceptionHandlerTrampolineBuilder(MaglevAssembler* masm)
      : masm_(masm) {}

  struct Move {
    explicit Move(const ValueLocation& target, ValueNode* source)
        : target(target), source(source) {}
    const ValueLocation& target;
    ValueNode* const source;
  };
  using MoveVector = base::SmallVector<Move, 16>;

  void EmitTrampolineFor(NodeBase* node) {
    DCHECK(node->properties().can_throw());

    ExceptionHandlerInfo* const handler_info = node->exception_handler_info();
    DCHECK(handler_info->HasExceptionHandler());
    BasicBlock* const catch_block = handler_info->catch_block.block_ptr();
    LazyDeoptInfo* const deopt_info = node->lazy_deopt_info();

    // The exception handler trampoline resolves moves for exception phis and
    // then jumps to the actual catch block. There are a few points worth
    // noting:
    //
    // - All source locations are assumed to be stack slots, except the
    // accumulator which is stored in kReturnRegister0. We don't emit an
    // explicit move for it, instead it is pushed and popped at the boundaries
    // of the entire move sequence (necessary due to materialisation).
    //
    // - Some values may require materialisation, i.e. heap number construction
    // through calls to the NewHeapNumber builtin. To avoid potential conflicts
    // with other moves (which may happen due to stack slot reuse, i.e. a
    // target location of move A may equal source location of move B), we
    // materialise and push results to new temporary stack slots before the
    // main move sequence, and then pop results into their final target
    // locations afterwards. Note this is only safe because a) materialised
    // values are tagged and b) the stack walk treats unknown stack slots as
    // tagged.

    const InterpretedDeoptFrame& lazy_frame =
        deopt_info->top_frame().type() ==
                DeoptFrame::FrameType::kBuiltinContinuationFrame
            ? deopt_info->top_frame().parent()->as_interpreted()
            : deopt_info->top_frame().as_interpreted();

    // TODO(v8:7700): Handle inlining.

    ParallelMoveResolver<Register> direct_moves(masm_);
    MoveVector materialising_moves;
    bool save_accumulator = false;
    RecordMoves(lazy_frame.unit(), catch_block, lazy_frame.frame_state(),
                &direct_moves, &materialising_moves, &save_accumulator);

    __ BindJumpTarget(&handler_info->trampoline_entry);
    __ RecordComment("-- Exception handler trampoline START");
    EmitMaterialisationsAndPushResults(materialising_moves, save_accumulator);
    __ RecordComment("EmitMoves");
    direct_moves.EmitMoves();
    EmitPopMaterialisedResults(materialising_moves, save_accumulator);
    __ Jump(catch_block->label());
    __ RecordComment("-- Exception handler trampoline END");
  }

  MaglevAssembler* masm() const { return masm_; }

  void RecordMoves(const MaglevCompilationUnit& unit, BasicBlock* catch_block,
                   const CompactInterpreterFrameState* register_frame,
                   ParallelMoveResolver<Register>* direct_moves,
                   MoveVector* materialising_moves, bool* save_accumulator) {
    for (Phi* phi : *catch_block->phis()) {
      DCHECK(phi->is_exception_phi());
      if (!phi->has_valid_live_range()) continue;

      const ValueLocation& target = phi->result();
      if (phi->owner() == interpreter::Register::virtual_accumulator()) {
        // If the accumulator is live, then it is the exception object located
        // at kReturnRegister0.  We don't emit a move for it since the value is
        // already in the right spot, but we do have to ensure it isn't
        // clobbered by calls to the NewHeapNumber builtin during
        // materialisation.
        DCHECK_EQ(target.AssignedGeneralRegister(), kReturnRegister0);
        *save_accumulator = true;
        continue;
      }

      ValueNode* const source = register_frame->GetValueOf(phi->owner(), unit);
      DCHECK_NOT_NULL(source);
      // All registers must have been spilled due to the call.
      // TODO(jgruber): Which call? Because any throw requires at least a call
      // to Runtime::kThrowFoo?
      DCHECK(!source->allocation().IsRegister());

      switch (source->properties().value_representation()) {
        case ValueRepresentation::kWord64:
          UNREACHABLE();
        case ValueRepresentation::kTagged:
          direct_moves->RecordMove(
              source, source->allocation(),
              compiler::AllocatedOperand::cast(target.operand()));
          break;
        case ValueRepresentation::kInt32:
        case ValueRepresentation::kUint32:
          materialising_moves->emplace_back(target, source);
          break;
        case ValueRepresentation::kFloat64:
          materialising_moves->emplace_back(target, source);
          break;
      }
    }
  }

  void EmitMaterialisationsAndPushResults(const MoveVector& moves,
                                          bool save_accumulator) const {
    if (moves.size() == 0) return;

    // It's possible to optimize this further, at the cost of additional
    // complexity:
    //
    // - If the target location is a register, we could theoretically move the
    // materialised result there immediately, with the additional complication
    // that following calls to NewHeapNumber may clobber the register.
    //
    // - If the target location is a stack slot which is neither a source nor
    // target slot for any other moves (direct or materialising), we could move
    // the result there directly instead of pushing and later popping it. This
    // doesn't seem worth the extra code complexity though, given we are
    // talking about a presumably infrequent case for exception handlers.

    __ RecordComment("EmitMaterialisationsAndPushResults");

    if (save_accumulator) __ Push(kReturnRegister0);
    for (const Move& move : moves) {
      // We consider constants after all other operations, since constants
      // don't need to call NewHeapNumber.
      if (IsConstantNode(move.source->opcode())) continue;
      __ MaterialiseValueNode(kReturnRegister0, move.source);
      __ Push(kReturnRegister0);
    }
  }

  void EmitPopMaterialisedResults(const MoveVector& moves,
                                  bool save_accumulator) const {
    if (moves.size() == 0) return;
    __ RecordComment("EmitPopMaterialisedResults");
    for (const Move& move : base::Reversed(moves)) {
      const ValueLocation& target = move.target;
      Register target_reg = target.operand().IsAnyRegister()
                                ? target.AssignedGeneralRegister()
                                : kScratchRegister;
      if (IsConstantNode(move.source->opcode())) {
        __ MaterialiseValueNode(target_reg, move.source);
      } else {
        __ Pop(target_reg);
      }
      if (target_reg == kScratchRegister) {
        __ Move(masm_->ToMemOperand(target.operand()), kScratchRegister);
      }
    }
    if (save_accumulator) __ Pop(kReturnRegister0);
  }

  MaglevAssembler* const masm_;
};

class MaglevCodeGeneratingNodeProcessor {
 public:
  explicit MaglevCodeGeneratingNodeProcessor(MaglevAssembler* masm)
      : masm_(masm) {}

  void PreProcessGraph(Graph* graph) {
    code_gen_state()->set_untagged_slots(graph->untagged_stack_slots());
    code_gen_state()->set_tagged_slots(graph->tagged_stack_slots());

    if (v8_flags.maglev_break_on_entry) {
      __ DebugBreak();
    }

    __ Prologue(graph);
  }

  void PostProcessGraph(Graph* graph) {}

  void PreProcessBasicBlock(BasicBlock* block) {
    if (v8_flags.code_comments) {
      std::stringstream ss;
      ss << "-- Block b" << graph_labeller()->BlockId(block);
      __ RecordComment(ss.str());
    }
    __ BindBlock(block);
  }

  template <typename NodeT>
  void Process(NodeT* node, const ProcessingState& state) {
    if (v8_flags.code_comments) {
      std::stringstream ss;
      ss << "--   " << graph_labeller()->NodeId(node) << ": "
         << PrintNode(graph_labeller(), node);
      __ RecordComment(ss.str());
    }

    __ AssertStackSizeCorrect();

    // Emit Phi moves before visiting the control node.
    if (std::is_base_of<UnconditionalControlNode, NodeT>::value) {
      EmitBlockEndGapMoves(node->template Cast<UnconditionalControlNode>(),
                           state);
    }

    if (v8_flags.debug_code) {
      // Check that all int32/uint32 inputs are zero extended
      for (Input& input : *node) {
        ValueRepresentation rep =
            input.node()->properties().value_representation();
        if (rep == ValueRepresentation::kInt32 ||
            rep == ValueRepresentation::kUint32) {
          // TODO(leszeks): Ideally we'd check non-register inputs too, but
          // AssertZeroExtended needs the scratch register, so we'd have to do
          // some manual push/pop here to free up another register.
          if (input.IsGeneralRegister()) {
            __ AssertZeroExtended(ToRegister(input));
          }
        }
      }
    }
    node->GenerateCode(masm(), state);

    if (std::is_base_of<ValueNode, NodeT>::value) {
      ValueNode* value_node = node->template Cast<ValueNode>();
      if (value_node->is_spilled()) {
        compiler::AllocatedOperand source =
            compiler::AllocatedOperand::cast(value_node->result().operand());
        // We shouldn't spill nodes which already output to the stack.
        if (!source.IsAnyStackSlot()) {
          if (v8_flags.code_comments) __ RecordComment("--   Spill:");
          if (source.IsRegister()) {
            __ Move(masm()->GetStackSlot(value_node->spill_slot()),
                    ToRegister(source));
          } else {
            __ Move(masm()->GetStackSlot(value_node->spill_slot()),
                    ToDoubleRegister(source));
          }
        } else {
          // Otherwise, the result source stack slot should be equal to the
          // spill slot.
          DCHECK_EQ(source.index(), value_node->spill_slot().index());
        }
      }
    }
  }

  void EmitBlockEndGapMoves(UnconditionalControlNode* node,
                            const ProcessingState& state) {
    BasicBlock* target = node->target();
    if (!target->has_state()) {
      __ RecordComment("--   Target has no state, must be a fallthrough");
      return;
    }

    int predecessor_id = state.block()->predecessor_id();

    // TODO(leszeks): Move these to fields, to allow their data structure
    // allocations to be reused. Will need some sort of state resetting.
    ParallelMoveResolver<Register> register_moves(masm_);
    ParallelMoveResolver<DoubleRegister> double_register_moves(masm_);

    // Remember what registers were assigned to by a Phi, to avoid clobbering
    // them with RegisterMoves.
    RegList registers_set_by_phis;

    __ RecordComment("--   Gap moves:");

    if (target->has_phi()) {
      Phi::List* phis = target->phis();
      for (Phi* phi : *phis) {
        // Ignore dead phis.
        // TODO(leszeks): We should remove dead phis entirely and turn this into
        // a DCHECK.
        if (!phi->has_valid_live_range()) {
          if (v8_flags.code_comments) {
            std::stringstream ss;
            ss << "--   * "
               << phi->input(state.block()->predecessor_id()).operand() << " → "
               << target << " (n" << graph_labeller()->NodeId(phi)
               << ") [DEAD]";
            __ RecordComment(ss.str());
          }
          continue;
        }
        Input& input = phi->input(state.block()->predecessor_id());
        ValueNode* node = input.node();
        compiler::InstructionOperand source = input.operand();
        compiler::AllocatedOperand target =
            compiler::AllocatedOperand::cast(phi->result().operand());
        if (v8_flags.code_comments) {
          std::stringstream ss;
          ss << "--   * " << source << " → " << target << " (n"
             << graph_labeller()->NodeId(phi) << ")";
          __ RecordComment(ss.str());
        }
        register_moves.RecordMove(node, source, target);
        if (target.IsAnyRegister()) {
          registers_set_by_phis.set(target.GetRegister());
        }
      }
    }

    target->state()->register_state().ForEachGeneralRegister(
        [&](Register reg, RegisterState& state) {
          // Don't clobber registers set by a Phi.
          if (registers_set_by_phis.has(reg)) return;

          ValueNode* node;
          RegisterMerge* merge;
          if (LoadMergeState(state, &node, &merge)) {
            compiler::InstructionOperand source =
                merge->operand(predecessor_id);
            if (v8_flags.code_comments) {
              std::stringstream ss;
              ss << "--   * " << source << " → " << reg;
              __ RecordComment(ss.str());
            }
            register_moves.RecordMove(node, source, reg);
          }
        });

    register_moves.EmitMoves();

    __ RecordComment("--   Double gap moves:");

    target->state()->register_state().ForEachDoubleRegister(
        [&](DoubleRegister reg, RegisterState& state) {
          ValueNode* node;
          RegisterMerge* merge;
          if (LoadMergeState(state, &node, &merge)) {
            compiler::InstructionOperand source =
                merge->operand(predecessor_id);
            if (v8_flags.code_comments) {
              std::stringstream ss;
              ss << "--   * " << source << " → " << reg;
              __ RecordComment(ss.str());
            }
            double_register_moves.RecordMove(node, source, reg);
          }
        });

    double_register_moves.EmitMoves();
  }

  Isolate* isolate() const { return masm_->isolate(); }
  MaglevAssembler* masm() const { return masm_; }
  MaglevCodeGenState* code_gen_state() const {
    return masm()->code_gen_state();
  }
  MaglevGraphLabeller* graph_labeller() const {
    return code_gen_state()->graph_labeller();
  }

 private:
  MaglevAssembler* const masm_;
};

class SafepointingNodeProcessor {
 public:
  explicit SafepointingNodeProcessor(LocalIsolate* local_isolate)
      : local_isolate_(local_isolate) {}

  void PreProcessGraph(Graph* graph) {}
  void PostProcessGraph(Graph* graph) {}
  void PreProcessBasicBlock(BasicBlock* block) {}
  void Process(NodeBase* node, const ProcessingState& state) {
    local_isolate_->heap()->Safepoint();
  }

 private:
  LocalIsolate* local_isolate_;
};

namespace {
int GetFrameCount(const DeoptFrame& deopt_frame) {
  switch (deopt_frame.type()) {
    case DeoptFrame::FrameType::kInterpretedFrame:
      return 1 + deopt_frame.as_interpreted().unit().inlining_depth();
    case DeoptFrame::FrameType::kBuiltinContinuationFrame:
      return 1 + GetFrameCount(*deopt_frame.parent());
  }
}
BytecodeOffset GetBytecodeOffset(const DeoptFrame& deopt_frame) {
  switch (deopt_frame.type()) {
    case DeoptFrame::FrameType::kInterpretedFrame:
      return deopt_frame.as_interpreted().bytecode_position();
    case DeoptFrame::FrameType::kBuiltinContinuationFrame:
      return Builtins::GetContinuationBytecodeOffset(
          deopt_frame.as_builtin_continuation().builtin_id());
  }
}
SourcePosition GetSourcePosition(const DeoptFrame& deopt_frame) {
  switch (deopt_frame.type()) {
    case DeoptFrame::FrameType::kInterpretedFrame:
      return deopt_frame.as_interpreted().source_position();
    case DeoptFrame::FrameType::kBuiltinContinuationFrame:
      return SourcePosition::Unknown();
  }
}
}  // namespace

class MaglevTranslationArrayBuilder {
 public:
  MaglevTranslationArrayBuilder(
      LocalIsolate* local_isolate, MaglevAssembler* masm,
      TranslationArrayBuilder* translation_array_builder,
      IdentityMap<int, base::DefaultAllocationPolicy>* deopt_literals)
      : local_isolate_(local_isolate),
        masm_(masm),
        translation_array_builder_(translation_array_builder),
        deopt_literals_(deopt_literals) {}

  void BuildEagerDeopt(EagerDeoptInfo* deopt_info) {
    int frame_count = GetFrameCount(deopt_info->top_frame());
    int jsframe_count = frame_count;
    int update_feedback_count =
        deopt_info->feedback_to_update().IsValid() ? 1 : 0;
    deopt_info->set_translation_index(
        translation_array_builder_->BeginTranslation(frame_count, jsframe_count,
                                                     update_feedback_count));
    if (deopt_info->feedback_to_update().IsValid()) {
      translation_array_builder_->AddUpdateFeedback(
          GetDeoptLiteral(*deopt_info->feedback_to_update().vector),
          deopt_info->feedback_to_update().index());
    }

    const InputLocation* current_input_location = deopt_info->input_locations();
    BuildDeoptFrame(deopt_info->top_frame(), current_input_location);
  }

  void BuildLazyDeopt(LazyDeoptInfo* deopt_info) {
    int frame_count = GetFrameCount(deopt_info->top_frame());
    int jsframe_count = frame_count;
    int update_feedback_count =
        deopt_info->feedback_to_update().IsValid() ? 1 : 0;
    deopt_info->set_translation_index(
        translation_array_builder_->BeginTranslation(frame_count, jsframe_count,
                                                     update_feedback_count));
    if (deopt_info->feedback_to_update().IsValid()) {
      translation_array_builder_->AddUpdateFeedback(
          GetDeoptLiteral(*deopt_info->feedback_to_update().vector),
          deopt_info->feedback_to_update().index());
    }

    const InputLocation* current_input_location = deopt_info->input_locations();

    if (deopt_info->top_frame().parent()) {
      // Deopt input locations are in the order of deopt frame emission, so
      // update the pointer after emitting the parent frame.
      BuildDeoptFrame(*deopt_info->top_frame().parent(),
                      current_input_location);
    }

    const DeoptFrame& top_frame = deopt_info->top_frame();
    switch (top_frame.type()) {
      case DeoptFrame::FrameType::kInterpretedFrame: {
        const InterpretedDeoptFrame& interpreted_frame =
            top_frame.as_interpreted();

        // Return offsets are counted from the end of the translation frame,
        // which is the array [parameters..., locals..., accumulator]. Since
        // it's the end, we don't need to worry about earlier frames.
        int return_offset;
        if (deopt_info->result_location() ==
            interpreter::Register::virtual_accumulator()) {
          return_offset = 0;
        } else if (deopt_info->result_location().is_parameter()) {
          // This is slightly tricky to reason about because of zero indexing
          // and fence post errors. As an example, consider a frame with 2
          // locals and 2 parameters, where we want argument index 1 -- looking
          // at the array in reverse order we have:
          //   [acc, r1, r0, a1, a0]
          //                  ^
          // and this calculation gives, correctly:
          //   2 + 2 - 1 = 3
          return_offset = interpreted_frame.unit().register_count() +
                          interpreted_frame.unit().parameter_count() -
                          deopt_info->result_location().ToParameterIndex();
        } else {
          return_offset = interpreted_frame.unit().register_count() -
                          deopt_info->result_location().index();
        }
        translation_array_builder_->BeginInterpretedFrame(
            interpreted_frame.bytecode_position(),
            GetDeoptLiteral(
                *interpreted_frame.unit().shared_function_info().object()),
            interpreted_frame.unit().register_count(), return_offset,
            deopt_info->result_size());

        BuildDeoptFrameValues(
            interpreted_frame.unit(), interpreted_frame.frame_state(),
            current_input_location, deopt_info->result_location(),
            deopt_info->result_size());
        break;
      }
      case DeoptFrame::FrameType::kBuiltinContinuationFrame: {
        const BuiltinContinuationDeoptFrame& builtin_continuation_frame =
            top_frame.as_builtin_continuation();

        translation_array_builder_->BeginBuiltinContinuationFrame(
            Builtins::GetContinuationBytecodeOffset(
                builtin_continuation_frame.builtin_id()),
            GetDeoptLiteral(*builtin_continuation_frame.parent()
                                 ->as_interpreted()
                                 .unit()
                                 .shared_function_info()
                                 .object()),
            builtin_continuation_frame.parameters().length());

        // Closure
        translation_array_builder_->StoreOptimizedOut();

        // Parameters
        for (ValueNode* value : builtin_continuation_frame.parameters()) {
          BuildDeoptFrameSingleValue(value, *current_input_location);
          current_input_location++;
        }

        // Context
        ValueNode* value = builtin_continuation_frame.context();
        BuildDeoptFrameSingleValue(value, *current_input_location);
        current_input_location++;
      }
    }
  }

 private:
  constexpr int DeoptStackSlotIndexFromFPOffset(int offset) {
    return 1 - offset / kSystemPointerSize;
  }

  int DeoptStackSlotFromStackSlot(const compiler::AllocatedOperand& operand) {
    return DeoptStackSlotIndexFromFPOffset(
        masm_->GetFramePointerOffsetForStackSlot(operand));
  }

  bool InReturnValues(interpreter::Register reg,
                      interpreter::Register result_location, int result_size) {
    if (result_size == 0 || !result_location.is_valid()) {
      return false;
    }
    return base::IsInRange(reg.index(), result_location.index(),
                           result_location.index() + result_size - 1);
  }

  void BuildDeoptFrame(const DeoptFrame& frame,
                       const InputLocation*& current_input_location) {
    if (frame.parent()) {
      // Deopt input locations are in the order of deopt frame emission, so
      // update the pointer after emitting the parent frame.
      BuildDeoptFrame(*frame.parent(), current_input_location);
    }

    switch (frame.type()) {
      case DeoptFrame::FrameType::kInterpretedFrame: {
        const InterpretedDeoptFrame& interpreted_frame = frame.as_interpreted();
        // Returns are used for updating an accumulator or register after a
        // lazy deopt.
        const int return_offset = 0;
        const int return_count = 0;
        translation_array_builder_->BeginInterpretedFrame(
            interpreted_frame.bytecode_position(),
            GetDeoptLiteral(
                *interpreted_frame.unit().shared_function_info().object()),
            interpreted_frame.unit().register_count(), return_offset,
            return_count);

        BuildDeoptFrameValues(
            interpreted_frame.unit(), interpreted_frame.frame_state(),
            current_input_location, interpreter::Register::invalid_value(),
            return_count);
        break;
      }
      case DeoptFrame::FrameType::kBuiltinContinuationFrame: {
        const BuiltinContinuationDeoptFrame& builtin_continuation_frame =
            frame.as_builtin_continuation();

        translation_array_builder_->BeginBuiltinContinuationFrame(
            Builtins::GetContinuationBytecodeOffset(
                builtin_continuation_frame.builtin_id()),
            GetDeoptLiteral(*builtin_continuation_frame.parent()
                                 ->as_interpreted()
                                 .unit()
                                 .shared_function_info()
                                 .object()),
            builtin_continuation_frame.parameters().length());

        // Closure
        translation_array_builder_->StoreOptimizedOut();

        // Parameters
        for (ValueNode* value : builtin_continuation_frame.parameters()) {
          BuildDeoptFrameSingleValue(value, *current_input_location);
          current_input_location++;
        }

        // Context
        ValueNode* value = builtin_continuation_frame.context();
        BuildDeoptFrameSingleValue(value, *current_input_location);
        current_input_location++;

        break;
      }
    }
  }

  void BuildDeoptStoreRegister(const compiler::AllocatedOperand& operand,
                               ValueRepresentation repr) {
    switch (repr) {
      case ValueRepresentation::kWord64:
        UNREACHABLE();
      case ValueRepresentation::kTagged:
        translation_array_builder_->StoreRegister(operand.GetRegister());
        break;
      case ValueRepresentation::kInt32:
        translation_array_builder_->StoreInt32Register(operand.GetRegister());
        break;
      case ValueRepresentation::kUint32:
        translation_array_builder_->StoreUint32Register(operand.GetRegister());
        break;
      case ValueRepresentation::kFloat64:
        translation_array_builder_->StoreDoubleRegister(
            operand.GetDoubleRegister());
        break;
    }
  }

  void BuildDeoptStoreStackSlot(const compiler::AllocatedOperand& operand,
                                ValueRepresentation repr) {
    int stack_slot = DeoptStackSlotFromStackSlot(operand);
    switch (repr) {
      case ValueRepresentation::kWord64:
        UNREACHABLE();
      case ValueRepresentation::kTagged:
        translation_array_builder_->StoreStackSlot(stack_slot);
        break;
      case ValueRepresentation::kInt32:
        translation_array_builder_->StoreInt32StackSlot(stack_slot);
        break;
      case ValueRepresentation::kUint32:
        translation_array_builder_->StoreUint32StackSlot(stack_slot);
        break;
      case ValueRepresentation::kFloat64:
        translation_array_builder_->StoreDoubleStackSlot(stack_slot);
        break;
    }
  }

  void BuildDeoptFrameSingleValue(ValueNode* value,
                                  const InputLocation& input_location) {
    if (input_location.operand().IsConstant()) {
      translation_array_builder_->StoreLiteral(
          GetDeoptLiteral(*value->Reify(local_isolate_)));
    } else {
      const compiler::AllocatedOperand& operand =
          compiler::AllocatedOperand::cast(input_location.operand());
      ValueRepresentation repr = value->properties().value_representation();
      if (operand.IsAnyRegister()) {
        BuildDeoptStoreRegister(operand, repr);
      } else {
        BuildDeoptStoreStackSlot(operand, repr);
      }
    }
  }

  void BuildDeoptFrameValues(
      const MaglevCompilationUnit& compilation_unit,
      const CompactInterpreterFrameState* checkpoint_state,
      const InputLocation*& input_location,
      interpreter::Register result_location, int result_size) {
    // Closure
    if (compilation_unit.inlining_depth() == 0) {
      int closure_index = DeoptStackSlotIndexFromFPOffset(
          StandardFrameConstants::kFunctionOffset);
      translation_array_builder_->StoreStackSlot(closure_index);
    } else {
      translation_array_builder_->StoreLiteral(
          GetDeoptLiteral(*compilation_unit.function().object()));
    }

    // TODO(leszeks): The input locations array happens to be in the same order
    // as parameters+context+locals+accumulator are accessed here. We should
    // make this clearer and guard against this invariant failing.

    // Parameters
    {
      int i = 0;
      checkpoint_state->ForEachParameter(
          compilation_unit, [&](ValueNode* value, interpreter::Register reg) {
            DCHECK_EQ(reg.ToParameterIndex(), i);
            if (InReturnValues(reg, result_location, result_size)) {
              translation_array_builder_->StoreOptimizedOut();
            } else {
              BuildDeoptFrameSingleValue(value, *input_location);
              input_location++;
            }
            i++;
          });
    }

    // Context
    ValueNode* value = checkpoint_state->context(compilation_unit);
    BuildDeoptFrameSingleValue(value, *input_location);
    input_location++;

    // Locals
    {
      int i = 0;
      checkpoint_state->ForEachLocal(
          compilation_unit, [&](ValueNode* value, interpreter::Register reg) {
            DCHECK_LE(i, reg.index());
            if (InReturnValues(reg, result_location, result_size)) return;
            while (i < reg.index()) {
              translation_array_builder_->StoreOptimizedOut();
              i++;
            }
            DCHECK_EQ(i, reg.index());
            BuildDeoptFrameSingleValue(value, *input_location);
            input_location++;
            i++;
          });
      while (i < compilation_unit.register_count()) {
        translation_array_builder_->StoreOptimizedOut();
        i++;
      }
    }

    // Accumulator
    {
      if (checkpoint_state->liveness()->AccumulatorIsLive() &&
          !InReturnValues(interpreter::Register::virtual_accumulator(),
                          result_location, result_size)) {
        ValueNode* value = checkpoint_state->accumulator(compilation_unit);
        BuildDeoptFrameSingleValue(value, *input_location);
        input_location++;
      } else {
        translation_array_builder_->StoreOptimizedOut();
      }
    }
  }

  int GetDeoptLiteral(Object obj) {
    IdentityMapFindResult<int> res = deopt_literals_->FindOrInsert(obj);
    if (!res.already_exists) {
      DCHECK_EQ(0, *res.entry);
      *res.entry = deopt_literals_->size() - 1;
    }
    return *res.entry;
  }

  LocalIsolate* local_isolate_;
  MaglevAssembler* masm_;
  TranslationArrayBuilder* translation_array_builder_;
  IdentityMap<int, base::DefaultAllocationPolicy>* deopt_literals_;
};

}  // namespace

MaglevCodeGenerator::MaglevCodeGenerator(
    LocalIsolate* isolate, MaglevCompilationInfo* compilation_info,
    Graph* graph)
    : local_isolate_(isolate),
      safepoint_table_builder_(compilation_info->zone(),
                               graph->tagged_stack_slots(),
                               graph->untagged_stack_slots()),
      translation_array_builder_(compilation_info->zone()),
      code_gen_state_(compilation_info, &safepoint_table_builder_),
      masm_(isolate->GetMainThreadIsolateUnsafe(), &code_gen_state_),
      graph_(graph),
      deopt_literals_(isolate->heap()->heap()) {}

void MaglevCodeGenerator::Assemble() {
  EmitCode();
  EmitMetadata();
}

MaybeHandle<Code> MaglevCodeGenerator::Generate(Isolate* isolate) {
  return BuildCodeObject(isolate);
}

void MaglevCodeGenerator::EmitCode() {
  GraphProcessor<NodeMultiProcessor<SafepointingNodeProcessor,
                                    MaglevCodeGeneratingNodeProcessor>>
      processor(SafepointingNodeProcessor{local_isolate_},
                MaglevCodeGeneratingNodeProcessor{masm()});
  processor.ProcessGraph(graph_);
  EmitDeferredCode();
  EmitDeopts();
  EmitExceptionHandlerTrampolines();
  __ FinishCode();
}

void MaglevCodeGenerator::EmitDeferredCode() {
  // Loop over deferred_code() multiple times, clearing the vector on each
  // outer loop, so that deferred code can itself emit deferred code.
  while (!code_gen_state_.deferred_code().empty()) {
    for (DeferredCodeInfo* deferred_code : code_gen_state_.TakeDeferredCode()) {
      __ RecordComment("-- Deferred block");
      __ bind(&deferred_code->deferred_code_label);
      deferred_code->Generate(masm());
      __ Trap();
    }
  }
}

void MaglevCodeGenerator::EmitDeopts() {
  MaglevTranslationArrayBuilder translation_builder(
      local_isolate_, &masm_, &translation_array_builder_, &deopt_literals_);

  // Deoptimization exits must be as small as possible, since their count grows
  // with function size. These labels are an optimization which extracts the
  // (potentially large) instruction sequence for the final jump to the
  // deoptimization entry into a single spot per Code object. All deopt exits
  // can then near-call to this label. Note: not used on all architectures.
  Label eager_deopt_entry;
  Label lazy_deopt_entry;
  __ MaybeEmitDeoptBuiltinsCall(
      code_gen_state_.eager_deopts().size(), &eager_deopt_entry,
      code_gen_state_.lazy_deopts().size(), &lazy_deopt_entry);

  deopt_exit_start_offset_ = __ pc_offset();

  int deopt_index = 0;

  __ RecordComment("-- Non-lazy deopts");
  for (EagerDeoptInfo* deopt_info : code_gen_state_.eager_deopts()) {
    local_isolate_->heap()->Safepoint();
    translation_builder.BuildEagerDeopt(deopt_info);

    if (masm_.compilation_info()->collect_source_positions() ||
        IsDeoptimizationWithoutCodeInvalidation(deopt_info->reason())) {
      // Note: Maglev uses the deopt_reason to tell the deoptimizer not to
      // discard optimized code on deopt during ML-TF OSR. This is why we
      // unconditionally emit the deopt_reason when
      // IsDeoptimizationWithoutCodeInvalidation is true.
      __ RecordDeoptReason(deopt_info->reason(), 0,
                           GetSourcePosition(deopt_info->top_frame()),
                           deopt_index);
    }
    __ bind(deopt_info->deopt_entry_label());

    __ CallForDeoptimization(Builtin::kDeoptimizationEntry_Eager, deopt_index,
                             deopt_info->deopt_entry_label(),
                             DeoptimizeKind::kEager, nullptr,
                             &eager_deopt_entry);

    deopt_index++;
  }

  __ RecordComment("-- Lazy deopts");
  int last_updated_safepoint = 0;
  for (LazyDeoptInfo* deopt_info : code_gen_state_.lazy_deopts()) {
    local_isolate_->heap()->Safepoint();
    translation_builder.BuildLazyDeopt(deopt_info);

    if (masm_.compilation_info()->collect_source_positions()) {
      __ RecordDeoptReason(DeoptimizeReason::kUnknown, 0,
                           GetSourcePosition(deopt_info->top_frame()),
                           deopt_index);
    }
    __ BindExceptionHandler(deopt_info->deopt_entry_label());

    __ CallForDeoptimization(Builtin::kDeoptimizationEntry_Lazy, deopt_index,
                             deopt_info->deopt_entry_label(),
                             DeoptimizeKind::kLazy, nullptr, &lazy_deopt_entry);

    last_updated_safepoint = safepoint_table_builder_.UpdateDeoptimizationInfo(
        deopt_info->deopting_call_return_pc(),
        deopt_info->deopt_entry_label()->pos(), last_updated_safepoint,
        deopt_index);
    deopt_index++;
  }
}

void MaglevCodeGenerator::EmitExceptionHandlerTrampolines() {
  if (code_gen_state_.handlers().size() == 0) return;
  __ RecordComment("-- Exception handler trampolines");
  for (NodeBase* node : code_gen_state_.handlers()) {
    ExceptionHandlerTrampolineBuilder::Build(masm(), node);
  }
}

void MaglevCodeGenerator::EmitMetadata() {
  // Final alignment before starting on the metadata section.
  masm()->Align(Code::kMetadataAlignment);

  safepoint_table_builder_.Emit(masm());

  // Exception handler table.
  handler_table_offset_ = HandlerTable::EmitReturnTableStart(masm());
  for (NodeBase* node : code_gen_state_.handlers()) {
    ExceptionHandlerInfo* info = node->exception_handler_info();
    HandlerTable::EmitReturnEntry(masm(), info->pc_offset,
                                  info->trampoline_entry.pos());
  }
}

MaybeHandle<Code> MaglevCodeGenerator::BuildCodeObject(Isolate* isolate) {
  CodeDesc desc;
  masm()->GetCode(isolate, &desc, &safepoint_table_builder_,
                  handler_table_offset_);
  return Factory::CodeBuilder{isolate, desc, CodeKind::MAGLEV}
      .set_stack_slots(stack_slot_count_with_fixed_frame())
      .set_deoptimization_data(GenerateDeoptimizationData(isolate))
      .TryBuild();
}

Handle<DeoptimizationData> MaglevCodeGenerator::GenerateDeoptimizationData(
    Isolate* isolate) {
  int eager_deopt_count =
      static_cast<int>(code_gen_state_.eager_deopts().size());
  int lazy_deopt_count = static_cast<int>(code_gen_state_.lazy_deopts().size());
  int deopt_count = lazy_deopt_count + eager_deopt_count;
  if (deopt_count == 0) {
    return DeoptimizationData::Empty(isolate);
  }
  Handle<DeoptimizationData> data =
      DeoptimizationData::New(isolate, deopt_count, AllocationType::kOld);

  Handle<TranslationArray> translation_array =
      translation_array_builder_.ToTranslationArray(isolate->factory());
  {
    DisallowGarbageCollection no_gc;
    auto raw_data = *data;

    raw_data.SetTranslationByteArray(*translation_array);
    // TODO(leszeks): Fix with the real inlined function count.
    raw_data.SetInlinedFunctionCount(Smi::zero());
    // TODO(leszeks): Support optimization IDs
    raw_data.SetOptimizationId(Smi::zero());

    DCHECK_NE(deopt_exit_start_offset_, -1);
    raw_data.SetDeoptExitStart(Smi::FromInt(deopt_exit_start_offset_));
    raw_data.SetEagerDeoptCount(Smi::FromInt(eager_deopt_count));
    raw_data.SetLazyDeoptCount(Smi::FromInt(lazy_deopt_count));

    raw_data.SetSharedFunctionInfo(*code_gen_state_.compilation_info()
                                        ->toplevel_compilation_unit()
                                        ->shared_function_info()
                                        .object());
  }

  Handle<DeoptimizationLiteralArray> literals =
      isolate->factory()->NewDeoptimizationLiteralArray(deopt_literals_.size() +
                                                        1);
  // TODO(leszeks): Fix with the real inlining positions.
  Handle<PodArray<InliningPosition>> inlining_positions =
      PodArray<InliningPosition>::New(isolate, 0);
  DisallowGarbageCollection no_gc;

  auto raw_literals = *literals;
  auto raw_data = *data;
  IdentityMap<int, base::DefaultAllocationPolicy>::IteratableScope iterate(
      &deopt_literals_);
  for (auto it = iterate.begin(); it != iterate.end(); ++it) {
    raw_literals.set(*it.entry(), it.key());
  }
  // Add the bytecode to the deopt literals to make sure it's held strongly.
  // TODO(leszeks): Do this for inlined functions too.
  raw_literals.set(deopt_literals_.size(), *code_gen_state_.compilation_info()
                                                ->toplevel_compilation_unit()
                                                ->bytecode()
                                                .object());
  raw_data.SetLiteralArray(raw_literals);

  // TODO(leszeks): Fix with the real inlining positions.
  raw_data.SetInliningPositions(*inlining_positions);

  // TODO(leszeks): Fix once we have OSR.
  BytecodeOffset osr_offset = BytecodeOffset::None();
  raw_data.SetOsrBytecodeOffset(Smi::FromInt(osr_offset.ToInt()));
  raw_data.SetOsrPcOffset(Smi::FromInt(-1));

  // Populate deoptimization entries.
  int i = 0;
  for (EagerDeoptInfo* deopt_info : code_gen_state_.eager_deopts()) {
    DCHECK_NE(deopt_info->translation_index(), -1);
    raw_data.SetBytecodeOffset(i, GetBytecodeOffset(deopt_info->top_frame()));
    raw_data.SetTranslationIndex(i,
                                 Smi::FromInt(deopt_info->translation_index()));
    raw_data.SetPc(i, Smi::FromInt(deopt_info->deopt_entry_label()->pos()));
#ifdef DEBUG
    raw_data.SetNodeId(i, Smi::FromInt(i));
#endif  // DEBUG
    i++;
  }
  for (LazyDeoptInfo* deopt_info : code_gen_state_.lazy_deopts()) {
    DCHECK_NE(deopt_info->translation_index(), -1);
    raw_data.SetBytecodeOffset(i, GetBytecodeOffset(deopt_info->top_frame()));
    raw_data.SetTranslationIndex(i,
                                 Smi::FromInt(deopt_info->translation_index()));
    raw_data.SetPc(i, Smi::FromInt(deopt_info->deopt_entry_label()->pos()));
#ifdef DEBUG
    raw_data.SetNodeId(i, Smi::FromInt(i));
#endif  // DEBUG
    i++;
  }

  return data;
}

}  // namespace maglev
}  // namespace internal
}  // namespace v8
