#include "macroassembler.h"

#include "code-space.h" // CodeSpace
#include "heap.h" // HeapValue
#include "heap-inl.h"

#include "stubs.h"
#include "utils.h" // ComputeHash

#include <stdlib.h> // NULL

namespace candor {
namespace internal {

Masm::Masm(CodeSpace* space) : slot_(rax, 0),
                               space_(space),
                               align_(0) {
}


void Masm::Pushad() {
  // 10 registers to save (10 * 8 = 16 * 5, so stack should be aligned)
  push(rax);
  push(rcx);
  push(rdx);
  push(rsi);
  push(rdi);
  push(r8);
  push(r9);
  // Root register
  push(root_reg);
  push(r12);

  // Last one just for alignment
  push(r15);
}


void Masm::Popad(Register preserve) {
  PreservePop(r15, preserve);
  PreservePop(r12, preserve);
  PreservePop(root_reg, preserve);
  PreservePop(r9, preserve);
  PreservePop(r8, preserve);
  PreservePop(rdi, preserve);
  PreservePop(rsi, preserve);
  PreservePop(rdx, preserve);
  PreservePop(rcx, preserve);
  PreservePop(rax, preserve);
}


void Masm::AlignCode() {
  offset_ = RoundUp(offset_, 16);
  Grow();
}


Masm::Align::Align(Masm* masm) : masm_(masm), align_(masm->align_) {
  if (align_ % 2 == 0) return;

  masm->push(Immediate(Heap::kTagNil));
  masm->align_ += 1;
}


Masm::Align::~Align() {
  if (align_ % 2 == 0) return;
  masm_->addq(rsp, 8);
  masm_->align_ -= 1;
}


Masm::Spill::Spill(Masm* masm, Register src) : masm_(masm),
                                               src_(src),
                                               index_(0) {
  index_ = masm->spill_index_++;
  Operand slot(rax, 0);
  masm->SpillSlot(index(), slot);
  masm->movq(slot, src);

  if (masm->spill_index_ > masm->spills_) masm->spills_ = masm->spill_index_;
}


Masm::Spill::~Spill() {
  masm()->spill_index_--;
}


void Masm::Spill::Unspill(Register dst) {
  Operand slot(rax, 0);
  masm()->SpillSlot(index(), slot);
  masm()->movq(dst, slot);
}


void Masm::Spill::Unspill() {
  return Unspill(src_);
}


void Masm::AllocateSpills(uint32_t stack_slots) {
  spill_offset_ = RoundUp((stack_slots + 1) * 8, 16);
  spills_ = 0;
  spill_index_ = 0;
  subq(rsp, Immediate(0));
  spill_reloc_ = new RelocationInfo(RelocationInfo::kValue,
                                    RelocationInfo::kLong,
                                    offset() - 4);

  relocation_info_.Push(spill_reloc_);
}


void Masm::FinalizeSpills() {
  spill_reloc_->target(spill_offset_ + RoundUp((spills_ + 1) << 3, 16));
}


void Masm::Allocate(Heap::HeapTag tag,
                    Register size_reg,
                    uint32_t size,
                    Register result) {
  Spill rax_s(this, rax);

  // Two arguments
  ChangeAlign(2);
  {
    Align a(this);

    // Add tag size
    if (size_reg.is(reg_nil)) {
      movq(rax, Immediate(TagNumber(size + 8)));
    } else {
      movq(rax, size_reg);
      Untag(rax);
      addq(rax, Immediate(8));
      TagNumber(rax);
    }
    push(rax);
    movq(rax, Immediate(TagNumber(tag)));
    push(rax);

    Call(stubs()->GetAllocateStub());
    // Stub will unwind stack
  }
  ChangeAlign(-2);

  if (!result.is(rax)) {
    movq(result, rax);
    rax_s.Unspill();
  }
}


void Masm::AllocateContext(uint32_t slots) {
  Spill rax_s(this, rax);

  // parent + number of slots + slots
  Allocate(Heap::kTagContext, reg_nil, 8 * (slots + 2), rax);

  // Move address of current context to first slot
  Operand qparent(rax, 8);
  movq(qparent, rdi);

  // Save number of slots
  Operand qslots(rax, 16);
  movq(qslots, Immediate(slots));

  // Clear context
  for (uint32_t i = 0; i < slots; i++) {
    Operand qslot(rax, 24 + i * 8);
    movq(qslot, Immediate(Heap::kTagNil));
  }

  // Replace current context
  // (It'll be restored by caller)
  movq(rdi, rax);
  rax_s.Unspill();

  CheckGC();
}


void Masm::AllocateFunction(Register addr, Register result) {
  // context + code
  Allocate(Heap::kTagFunction, reg_nil, 8 * 3, result);

  // Move address of current context to first slot
  Operand qparent(result, 8);
  Operand qaddr(result, 16);
  Operand qroot(result, 24);
  movq(qparent, rdi);
  movq(qaddr, addr);
  movq(qroot, root_reg);

  xorq(addr, addr);

  CheckGC();
}


void Masm::AllocateNumber(DoubleRegister value, Register result) {
  Allocate(Heap::kTagNumber, reg_nil, 8, result);

  Operand qvalue(result, 8);
  movqd(qvalue, value);

  CheckGC();
}


void Masm::AllocateObjectLiteral(Heap::HeapTag tag,
                                 Register size,
                                 Register result) {
  // mask + map
  Allocate(tag,
           reg_nil,
           tag == Heap::kTagArray ? 24 : 16,
           result);

  Operand qmask(result, 8);
  Operand qmap(result, HObject::kMapOffset);

  // Array only field
  Operand qlength(result, 24);

  // Set mask
  movq(scratch, size);

  // mask (= (size - 1) << 3)
  Untag(scratch);
  dec(scratch);
  shl(scratch, Immediate(3));
  movq(qmask, scratch);
  xorq(scratch, scratch);

  // Create map
  Spill size_s(this, size);

  Untag(size);
  // keys + values
  shl(size, Immediate(4));
  // + size
  addq(size, Immediate(8));
  TagNumber(size);

  Allocate(Heap::kTagMap, size, 0, scratch);
  movq(qmap, scratch);

  size_s.Unspill();
  Spill result_s(this, result);
  movq(result, scratch);

  // Save map size for GC
  Operand qmapsize(result, HMap::kSizeOffset);
  Untag(size);
  movq(qmapsize, size);

  // Fill map with nil
  shl(size, Immediate(4));
  addq(result, Immediate(16));
  addq(size, result);
  subq(size, Immediate(8));
  Fill(result, size, Immediate(Heap::kTagNil));

  result_s.Unspill();
  size_s.Unspill();

  // Set length
  if (tag == Heap::kTagArray) {
    movq(qlength, Immediate(0));
  }

  CheckGC();
}


void Masm::Fill(Register start, Register end, Immediate value) {
  Push(start);
  movq(scratch, value);

  Label entry(this), loop(this);
  jmp(&entry);
  bind(&loop);

  // Fill
  Operand op(start, 0);
  movq(op, scratch);

  // Move
  addq(start, Immediate(8));

  bind(&entry);

  // And loop
  cmpq(start, end);
  jmp(kLe, &loop);

  Pop(start);
  xorq(scratch, scratch);
}


void Masm::FillStackSlots() {
  movq(rax, rsp);
  movq(rbx, rbp);
  // Skip frame info
  subq(rbx, Immediate(8));
  Fill(rax, rbx, Immediate(Heap::kTagNil));
  xorq(rax, rax);
  xorq(rbx, rbx);
}


void Masm::EnterFramePrologue() {
  Immediate last_stack(reinterpret_cast<uint64_t>(heap()->last_stack()));
  Operand scratch_op(scratch, 0);

  movq(scratch, last_stack);
  push(scratch_op);
  push(Immediate(Heap::kEnterFrameTag));
}


void Masm::EnterFrameEpilogue() {
  addq(rsp, Immediate(16));
}


void Masm::ExitFramePrologue() {
  Immediate last_stack(reinterpret_cast<uint64_t>(heap()->last_stack()));
  Operand scratch_op(scratch, 0);

  movq(scratch, last_stack);
  push(scratch_op);
  push(Immediate(Heap::kTagNil));
  movq(scratch_op, rsp);
  xorq(scratch, scratch);
}


void Masm::ExitFrameEpilogue() {
  pop(scratch);
  pop(scratch);

  Immediate last_stack(reinterpret_cast<uint64_t>(heap()->last_stack()));
  Operand scratch_op(scratch, 0);

  // Restore previous last_stack
  push(rax);

  movq(rax, scratch);
  movq(scratch, last_stack);
  movq(scratch_op, rax);

  pop(rax);
}


void Masm::StringHash(Register str, Register result) {
  Operand hash_field(str, HString::kHashOffset);

  Label done(this);

  // Check if hash was already calculated
  movq(result, hash_field);
  cmpq(result, Immediate(0));
  jmp(kNe, &done);

  // Compute new hash
  assert(!str.is(rcx));
  if (!result.is(rcx)) push(rcx);
  push(str);
  push(rsi);

  Register scratch = rsi;

  // hash = 0
  xorq(result, result);

  // rcx = length
  Operand length_field(str, HString::kLengthOffset);
  movq(rcx, length_field);

  // str += kValueOffset
  addq(str, Immediate(HString::kValueOffset));

  // while (rcx != 0)
  Label loop_start(this), loop_cond(this), loop_end(this);

  jmp(&loop_cond);
  bind(&loop_start);

  Operand ch(str, 0);

  // result += str[0]
  movzxb(scratch, ch);
  addl(result, scratch);

  // result += result << 10
  movq(scratch, result);
  shll(result, Immediate(10));
  addl(result, scratch);

  // result ^= result >> 6
  movq(scratch, result);
  shrl(result, Immediate(6));
  xorl(result, scratch);

  // rcx--; str++
  dec(rcx);
  inc(str);

  bind(&loop_cond);

  // check condition (rcx != 0)
  cmpq(rcx, Immediate(0));
  jmp(kNe, &loop_start);

  bind(&loop_end);

  // Mixup
  // result += (result << 3);
  movq(scratch, result);
  shll(result, Immediate(3));
  addl(result, scratch);

  // result ^= (result >> 11);
  movq(scratch, result);
  shrl(result, Immediate(11));
  xorl(result, scratch);

  // result += (result << 15);
  movq(scratch, result);
  shll(result, Immediate(15));
  addl(result, scratch);

  pop(rsi);
  pop(str);
  if (!result.is(rcx)) pop(rcx);

  // Store hash into a string
  movq(hash_field, result);

  bind(&done);
}


void Masm::CheckGC() {
  Immediate gc_flag(reinterpret_cast<uint64_t>(heap()->needs_gc_addr()));
  Operand scratch_op(scratch, 0);

  Label done(this);

  // Check needs_gc flag
  movq(scratch, gc_flag);
  cmpb(scratch_op, Immediate(0));
  jmp(kEq, &done);

  Call(stubs()->GetCollectGarbageStub());

  bind(&done);
}


void Masm::IsNil(Register reference, Label* not_nil, Label* is_nil) {
  cmpq(reference, Immediate(Heap::kTagNil));
  if (is_nil != NULL) jmp(kEq, is_nil);
  if (not_nil != NULL) jmp(kNe, not_nil);
}


void Masm::IsUnboxed(Register reference, Label* not_unboxed, Label* unboxed) {
  testb(reference, Immediate(0x01));
  if (not_unboxed != NULL) jmp(kNe, not_unboxed);
  if (unboxed != NULL) jmp(kEq, unboxed);
}


void Masm::IsHeapObject(Heap::HeapTag tag,
                        Register reference,
                        Label* mismatch,
                        Label* match) {
  Operand qtag(reference, 0);
  cmpb(qtag, Immediate(tag));
  if (mismatch != NULL) jmp(kNe, mismatch);
  if (match != NULL) jmp(kEq, match);
}


void Masm::IsTrue(Register reference, Label* is_false, Label* is_true) {
  // reference is definitely a boolean value
  // so no need to check it's type here
  Operand bvalue(reference, 8);
  cmpb(bvalue, Immediate(0));
  if (is_false != NULL) jmp(kEq, is_false);
  if (is_true != NULL) jmp(kNe, is_true);
}


void Masm::Call(Register addr) {
  while ((offset() & 0x1) != 0x1) {
    nop();
  }
  callq(addr);
  nop();
}


void Masm::Call(Operand& addr) {
  while ((offset() & 0x1) != 0x1) {
    nop();
  }
  callq(addr);
  nop();
}


void Masm::Call(Register fn, uint32_t args) {
  Operand context_slot(fn, 8);
  Operand code_slot(fn, 16);
  Operand root_slot(fn, 24);

  Label binding(this), done(this);

  movq(rdi, context_slot);
  movq(rsi, Immediate(TagNumber(args)));
  movq(root_reg, root_slot);

  cmpq(rdi, Immediate(Heap::kBindingContextTag));
  jmp(kEq, &binding);

  Call(code_slot);

  jmp(&done);
  bind(&binding);

  push(rsi);
  push(fn);
  Call(stubs()->GetCallBindingStub());

  bind(&done);
}


void Masm::Call(char* stub) {
  movq(scratch, reinterpret_cast<uint64_t>(stub));

  Call(scratch);
}

} // namespace internal
} // namespace candor
