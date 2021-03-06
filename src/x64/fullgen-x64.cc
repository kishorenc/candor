#include "fullgen.h"
#include "macroassembler.h"
#include "code-space.h" // CodeSpace
#include "heap.h" // Heap
#include "heap-inl.h"
#include "ast.h" // AstNode
#include "zone.h" // ZoneObject
#include "stubs.h" // Stubs
#include "utils.h" // List

#include <assert.h>
#include <stdint.h> // uint32_t
#include <stdlib.h> // NULL
#include <stdio.h> // snprintf

namespace candor {
namespace internal {

void FFunction::Use(uint32_t offset) {
  RelocationInfo* info = new RelocationInfo(
        RelocationInfo::kAbsolute,
        RelocationInfo::kQuad,
        offset - 8);
  if (addr_ != 0) info->target(addr_);
  uses_.Push(info);
  masm()->relocation_info_.Push(info);
}


void FFunction::Allocate(uint32_t addr) {
  assert(addr_ == 0);
  addr_ = addr;
  List<RelocationInfo*, ZoneObject>::Item* item = uses_.head();
  while (item != NULL) {
    item->value()->target(addr);
    item = item->next();
  }
}


Fullgen::Fullgen(CodeSpace* space) : Masm(space),
                                     Visitor(kPreorder),
                                     space_(space),
                                     visitor_type_(kValue),
                                     current_function_(NULL),
                                     loop_start_(NULL),
                                     loop_end_(NULL),
                                     error_msg_(NULL),
                                     error_pos_(0) {
  // Create a `global` object
  root_context()->Push(HObject::NewEmpty(heap()));

  // Place some root values
  root_context()->Push(HBoolean::New(heap(), Heap::kTenureOld, true));
  root_context()->Push(HBoolean::New(heap(), Heap::kTenureOld, false));

  // Place types
  root_context()->Push(HString::New(heap(), Heap::kTenureOld, "nil", 3));
  root_context()->Push(HString::New(heap(), Heap::kTenureOld, "boolean", 7));
  root_context()->Push(HString::New(heap(), Heap::kTenureOld, "number", 6));
  root_context()->Push(HString::New(heap(), Heap::kTenureOld, "string", 6));
  root_context()->Push(HString::New(heap(), Heap::kTenureOld, "object", 6));
  root_context()->Push(HString::New(heap(), Heap::kTenureOld, "array", 5));
  root_context()->Push(HString::New(heap(), Heap::kTenureOld, "function", 8));
  root_context()->Push(HString::New(heap(), Heap::kTenureOld, "cdata", 5));
}


void Fullgen::CandorFunction::Generate() {
  // Generate function's body
  fullgen()->GeneratePrologue(fn());
  fullgen()->VisitChildren(fn());

  // In case if function doesn't have `return` statements
  // we should still return `nil` value
  masm()->movq(rax, Immediate(Heap::kTagNil));

  fullgen()->GenerateEpilogue(fn());
  fullgen()->FinalizeSpills();
}


void Fullgen::Throw(Heap::Error err) {
  assert(current_node() != NULL);
  SetError(Heap::ErrorToString(err), current_node()->offset());
  emitb(0xcc);
}


void Fullgen::Generate(AstNode* ast) {
  fns_.Push(new CandorFunction(this, FunctionLiteral::Cast(ast)));

  FFunction* fn;
  while ((fn = fns_.Shift()) != NULL) {
    current_function(CandorFunction::Cast(fn));

    // Align function if needed
    AlignCode();

    // Replace all function's uses by generated address
    fn->Allocate(offset());

    // Generate functions' body
    fn->Generate();
  }
}


void Fullgen::GeneratePrologue(AstNode* stmt) {
  // rdi <- reference to parent context (zero for root)
  // rsi <- unboxed arguments count (tagged)
  push(rbp);
  movq(rbp, rsp);

  // Allocate space for spill slots and on-stack variables
  AllocateSpills(stmt->stack_slots());

  FillStackSlots();

  // Allocate context and clear stack slots
  AllocateContext(stmt->context_slots());

  // Place all arguments into their slots
  Label body(this);

  FunctionLiteral* fn = FunctionLiteral::Cast(stmt);
  AstList::Item* item = fn->args()->head();
  uint32_t i = 0;
  while (item != NULL) {
    Operand lhs(rax, 0);
    Operand rhs(rbp, 8 * (2 + fn->args()->length() - ++i));

    cmpq(rsi, Immediate(TagNumber(i)));
    jmp(kLt, &body);

    VisitForSlot(item->value());
    movq(rdx, rhs);
    movq(slot(), rdx);

    item = item->next();
  }

  bind(&body);

  // Cleanup junk
  xorq(rdx, rdx);
}


void Fullgen::GenerateEpilogue(AstNode* stmt) {
  // rax will hold result of function
  movq(rsp, rbp);
  pop(rbp);

  ret(0);
}


void Fullgen::PlaceInRoot(char* addr) {
  Operand root_op(root_reg, 8 * (3 + root_context()->length()));
  movq(rax, root_op);

  root_context()->Push(addr);
}


char* Fullgen::AllocateRoot() {
  return HContext::New(heap(), root_context());
}


AstNode* Fullgen::VisitForValue(AstNode* node) {
  // Save previous data
  VisitorType stored_type = visitor_type_;

  // Set new
  visitor_type_ = kValue;

  // Visit node
  AstNode* result = Visit(node);

  // Restore
  visitor_type_ = stored_type;

  return result;
}


AstNode* Fullgen::VisitForSlot(AstNode* node) {
  // Save data
  VisitorType stored_type = visitor_type_;

  // Set new
  visitor_type_ = kSlot;

  // Visit node
  AstNode* result = Visit(node);

  // Restore
  visitor_type_ = stored_type;

  return result;
}


AstNode* Fullgen::VisitFunction(AstNode* stmt) {
  FunctionLiteral* fn = FunctionLiteral::Cast(stmt);
  FFunction* ffn = new CandorFunction(this, fn);
  fns_.Push(ffn);

  movq(rcx, Immediate(0));
  ffn->Use(offset());

  // Allocate function object that'll reference to current scope
  // and have address of actual code

  if (fn->variable() != NULL) {
    AllocateFunction(rcx, rdx);

    Spill rdx_s(this, rdx);

    AstNode* assign = new AstNode(AstNode::kAssign);
    assign->children()->Push(fn->variable());
    assign->children()->Push(new FAstSpill(&rdx_s));

    Visit(assign);
  } else {
    AllocateFunction(rcx, rax);
  }

  return stmt;
}


AstNode* Fullgen::VisitCall(AstNode* stmt) {
  FunctionLiteral* fn = FunctionLiteral::Cast(stmt);

  if (fn->variable() == NULL) {
    Throw(Heap::kErrorCallWithoutVariable);
    return stmt;
  }

  if (!visiting_for_value()) {
    Throw(Heap::kErrorIncorrectLhs);
    return stmt;
  }

  Label not_function(this), done(this);

  AstNode* name = AstValue::Cast(fn->variable())->name();

  // handle __$gc() call
  if (fn->variable()->is(AstNode::kValue) &&
      name->length() == 5 && strncmp(name->value(), "__$gc", 5) == 0) {
    Call(stubs()->GetCollectGarbageStub());
    movq(rax, Immediate(Heap::kTagNil));
    return stmt;
  }

  VisitForValue(fn->variable());

  Spill rax_s(this, rax);

  IsNil(rax, NULL, &not_function);
  IsUnboxed(rax, NULL, &not_function);
  IsHeapObject(Heap::kTagFunction, rax, &not_function, NULL);

  Spill rsi_s(this, rsi), rdi_s(this, rdi), root_s(this, root_reg);
  {
    ChangeAlign(fn->args()->length());
    Align a(this);
    ChangeAlign(-fn->args()->length());

    AstList::Item* item = fn->args()->head();
    while (item != NULL) {
      {
        Align a(this);
        VisitForValue(item->value());
      }

      // Push argument and change alignment
      push(rax);
      ChangeAlign(1);
      item = item->next();
    }
    // Restore alignment
    ChangeAlign(-fn->args()->length());

    // Generate calling code
    rax_s.Unspill();
    Call(rax, fn->args()->length());

    if (fn->args()->length() != 0) {
      // Unwind stack
      addq(rsp, Immediate(fn->args()->length() * 8));
    }
  }
  root_s.Unspill();
  rdi_s.Unspill();
  rsi_s.Unspill();

  jmp(&done);
  bind(&not_function);

  movq(rax, Immediate(Heap::kTagNil));

  bind(&done);

  return stmt;
}


AstNode* Fullgen::VisitAssign(AstNode* stmt) {
  Label done(this);

  // Get value of right-hand side expression in rbx
  VisitForValue(stmt->rhs());
  Spill rax_s(this, rax);

  // Get target slot for left-hand side
  VisitForSlot(stmt->lhs());

  rax_s.Unspill(scratch);

  // If slot's base is nil - just return value
  if (!slot().base().is(rbp)) {
    IsNil(slot().base(), NULL, &done);
  }

  // Put value into slot
  movq(slot(), scratch);

  // Propagate result of assign operation
  bind(&done);
  movq(rax, scratch);

  return stmt;
}


AstNode* Fullgen::VisitValue(AstNode* node) {
  AstValue* value = AstValue::Cast(node);

  // If it's Fullgen generated AST Value
  if (value->is_spill()) {
    assert(visiting_for_value());
    FAstSpill::Cast(value)->spill()->Unspill(rax);
    return node;
  }

  if (value->is_operand()) {
    slot().base(FAstOperand::Cast(value)->op()->base());
    slot().disp(FAstOperand::Cast(value)->op()->disp());

    if (visiting_for_value()) {
      movq(rax, slot());
    }

    return node;
  }

  slot().scale(Operand::one);

  // Get pointer to slot first
  if (value->slot()->is_stack()) {
    // On stack variables
    slot().base(rbp);
    slot().disp(-8 * (value->slot()->index() + 1));
  } else {
    int32_t depth = value->slot()->depth();

    if (depth == -2) {
      // Root register lookup
      slot().base(root_reg);
      slot().disp(8 * (value->slot()->index() + 3));
    } else if (depth == -1) {
      // Global lookup
      slot().base(root_reg);
      slot().disp(HContext::GetIndexDisp(Heap::kRootGlobalIndex));

      if (visiting_for_slot()) {
        Throw(Heap::kErrorIncorrectLhs);
      }
    } else {
      // Context variables
      movq(rax, rdi);

      // Lookup context
      while (--depth >= 0) {
        Operand parent(rax, 8);
        movq(rax, parent);
      }

      slot().base(rax);
      slot().scale(Operand::one);
      // Skip tag, code addr and reference to parent scope
      slot().disp(8 * (value->slot()->index() + 3));
    }
  }

  // If we was asked to return value - dereference slot
  if (visiting_for_value()) {
    movq(rax, slot());
  }

  return node;
}


AstNode* Fullgen::VisitMember(AstNode* node) {
  VisitForValue(node->lhs());
  Spill rax_s(this, rax);

  VisitForValue(node->rhs());
  movq(rbx, rax);
  rax_s.Unspill(rax);

  movq(rcx, Immediate(visiting_for_slot()));
  Call(stubs()->GetLookupPropertyStub());

  Label done(this);

  IsNil(rax, NULL, &done);

  rax_s.Unspill(rbx);

  Operand qmap(rbx, HObject::kMapOffset);
  movq(rbx, qmap);
  addq(rax, rbx);

  slot().base(rax);
  slot().disp(0);

  // Unbox value if asked
  if (visiting_for_value()) {
    movq(rax, slot());
  }

  bind(&done);

  slot().base(rax);
  slot().disp(0);

  return node;
}


AstNode* Fullgen::VisitNumber(AstNode* node) {
  if (!visiting_for_value()) {
    Throw(Heap::kErrorIncorrectLhs);
    return node;
  }

  if (StringIsDouble(node->value(), node->length())) {
    // Allocate boxed heap number
    double value = StringToDouble(node->value(), node->length());

    PlaceInRoot(HNumber::New(heap(), Heap::kTenureOld, value));
  } else {
    // Allocate unboxed number
    int64_t value = StringToInt(node->value(), node->length());
    movq(rax, Immediate(TagNumber(value)));
  }

  return node;
}


AstNode* Fullgen::VisitString(AstNode* node) {
  if (visiting_for_slot()) {
    Throw(Heap::kErrorIncorrectLhs);
    return node;
  }

  // Unescape string
  uint32_t length;
  const char* unescaped = Unescape(node->value(), node->length(), &length);

  PlaceInRoot(HString::New(heap(), Heap::kTenureOld, unescaped, length));

  delete unescaped;

  return node;
}


AstNode* Fullgen::VisitProperty(AstNode* node) {
  // kProperty is essentially the same as string
  return VisitString(node);
}


AstNode* Fullgen::VisitIf(AstNode* node) {
  Label fail_body(this), done(this);

  AstNode* expr = node->lhs();
  AstNode* success = node->rhs();
  AstList::Item* fail_item = node->children()->head()->next()->next();
  AstNode* fail = NULL;
  if (fail_item != NULL) fail = fail_item->value();

  VisitForValue(expr);

  Call(stubs()->GetCoerceToBooleanStub());

  IsTrue(rax, &fail_body, NULL);

  VisitForValue(success);

  jmp(&done);
  bind(&fail_body);

  if (fail != NULL) VisitForValue(fail);

  bind(&done);

  return node;
}


AstNode* Fullgen::VisitWhile(AstNode* node) {
  Label loop_start(this), loop_end(this);

  AstNode* expr = node->lhs();
  AstNode* body = node->rhs();

  LoopVisitor visitor(this, &loop_start, &loop_end);

  bind(&loop_start);

  VisitForValue(expr);

  Call(stubs()->GetCoerceToBooleanStub());

  IsTrue(rax, &loop_end, NULL);

  VisitForValue(body);

  jmp(&loop_start);

  bind(&loop_end);

  return node;
}


AstNode* Fullgen::VisitNil(AstNode* node) {
  if (visiting_for_slot()) {
    Throw(Heap::kErrorIncorrectLhs);
    return node;
  }

  movq(rax, Immediate(Heap::kTagNil));

  return node;
}


AstNode* Fullgen::VisitTrue(AstNode* node) {
  if (visiting_for_slot()) {
    Throw(Heap::kErrorIncorrectLhs);
    return node;
  }

  Operand true_slot(root_reg, HContext::GetIndexDisp(Heap::kRootTrueIndex));
  movq(rax, true_slot);

  return node;
}


AstNode* Fullgen::VisitFalse(AstNode* node) {
  if (visiting_for_slot()) {
    Throw(Heap::kErrorIncorrectLhs);
    return node;
  }

  Operand false_slot(root_reg, HContext::GetIndexDisp(Heap::kRootFalseIndex));
  movq(rax, false_slot);

  return node;
}


AstNode* Fullgen::VisitObjectLiteral(AstNode* node) {
  if (visiting_for_slot()) {
    Throw(Heap::kErrorIncorrectLhs);
    return node;
  }

  ObjectLiteral* obj = ObjectLiteral::Cast(node);

  // Ensure that map will be filled only by half at maximum
  movq(rbx, Immediate(TagNumber(PowerOfTwo(node->children()->length() << 1))));
  AllocateObjectLiteral(Heap::kTagObject, rbx, rdx);

  Spill rdx_s(this, rdx);

  // Set every key/value pair
  assert(obj->keys()->length() == obj->values()->length());
  AstList::Item* key = obj->keys()->head();
  AstList::Item* value = obj->values()->head();
  while (key != NULL) {
    AstNode* member = new AstNode(AstNode::kMember);
    member->children()->Push(new FAstSpill(&rdx_s));
    member->children()->Push(key->value());

    AstNode* assign = new AstNode(AstNode::kAssign);
    assign->children()->Push(member);
    assign->children()->Push(value->value());

    VisitForValue(assign);

    key = key->next();
    value = value->next();
  }

  rdx_s.Unspill(rax);

  return node;
}


AstNode* Fullgen::VisitArrayLiteral(AstNode* node) {
  if (visiting_for_slot()) {
    Throw(Heap::kErrorIncorrectLhs);
    return node;
  }

  // Ensure that map will be filled only by half at maximum
  movq(rbx,
       Immediate(TagNumber(PowerOfTwo(node->children()->length() << 1))));
  AllocateObjectLiteral(Heap::kTagArray, rbx, rdx);

  Spill rdx_s(this, rdx);

  AstList::Item* item = node->children()->head();
  uint64_t index = 0;
  while (item != NULL) {
    char keystr[32];
    AstNode* key = new AstNode(AstNode::kNumber);
    key->value(keystr);
    key->length(snprintf(keystr, sizeof(keystr), "%llu", index));

    AstNode* member = new AstNode(AstNode::kMember);
    member->children()->Push(new FAstSpill(&rdx_s));
    member->children()->Push(key);

    AstNode* assign = new AstNode(AstNode::kAssign);
    assign->children()->Push(member);
    assign->children()->Push(item->value());

    VisitForValue(assign);

    item = item->next();
    index++;
  }

  rdx_s.Unspill(rax);

  return node;
}


AstNode* Fullgen::VisitReturn(AstNode* node) {
  if (node->lhs() != NULL) {
    // Get value of expression
    VisitForValue(node->lhs());
  } else {
    // Or just nullify output
    movq(rax, Immediate(Heap::kTagNil));
  }

  GenerateEpilogue(current_function()->fn());

  return node;
}


AstNode* Fullgen::VisitNew(AstNode* node) {
  VisitForValue(node->lhs());
  Call(stubs()->GetCloneObjectStub());

  return node;
}


AstNode* Fullgen::VisitBreak(AstNode* node) {
  if (loop_end() == NULL) {
    Throw(Heap::kErrorExpectedLoop);
    return node;
  }

  jmp(loop_end());

  return node;
}


AstNode* Fullgen::VisitContinue(AstNode* node) {
  if (loop_start() == NULL) {
    Throw(Heap::kErrorExpectedLoop);
    return node;
  }

  jmp(loop_start());

  return node;
}


AstNode* Fullgen::VisitTypeof(AstNode* node) {
  if (visiting_for_slot()) {
    Throw(Heap::kErrorIncorrectLhs);
    return node;
  }

  Align a(this);

  VisitForValue(node->lhs());
  Call(stubs()->GetTypeofStub());

  return node;
}


AstNode* Fullgen::VisitSizeof(AstNode* node) {
  if (visiting_for_slot()) {
    Throw(Heap::kErrorIncorrectLhs);
    return node;
  }

  Align a(this);

  VisitForValue(node->lhs());
  Call(stubs()->GetSizeofStub());

  return node;
}


AstNode* Fullgen::VisitKeysof(AstNode* node) {
  if (visiting_for_slot()) {
    Throw(Heap::kErrorIncorrectLhs);
    return node;
  }

  Align a(this);

  VisitForValue(node->lhs());
  Call(stubs()->GetKeysofStub());

  return node;
}


AstNode* Fullgen::VisitUnOp(AstNode* node) {
  UnOp* op = UnOp::Cast(node);

  if (visiting_for_slot()) {
    Throw(Heap::kErrorIncorrectLhs);
    return node;
  }

  // Changing ops should be translated into another form
  if (op->is_changing()) {
    AstNode* rhs = NULL;
    AstNode* one = new AstNode(AstNode::kNumber);
    one->value("1");
    one->length(1);

    switch (op->subtype()) {
     case UnOp::kPreInc:
     case UnOp::kPostInc:
      rhs = new BinOp(BinOp::kAdd, op->lhs(), one);
      break;
     case UnOp::kPreDec:
     case UnOp::kPostDec:
      rhs = new BinOp(BinOp::kSub, op->lhs(), one);
      break;
     default:
      break;
    }

    AstNode* assign = new AstNode(AstNode::kAssign);
    assign->children()->Push(op->lhs());
    assign->children()->Push(rhs);

    // ++a => a = a + 1
    if (op->subtype() == UnOp::kPreInc || op->subtype() == UnOp::kPreDec) {
      Visit(assign);
      return node;
    }

    // a++ => $scratch = a; a = $scratch + 1; $scratch
    VisitForSlot(op->lhs());

    Label done(this), nil_result(this);

    // If slot's base is nil - just return value
    IsNil(slot().base(), NULL, &nil_result);

    // Get value
    movq(scratch, slot());
    Spill scratch_s(this, scratch);

    // Put slot's value into rbx
    movq(rbx, slot());

    Spill rbx_s(this, rbx);

    assign->children()->head()->value(new FAstOperand(&slot()));
    rhs->children()->head()->value(new FAstSpill(&rbx_s));
    VisitForValue(assign);

    scratch_s.Unspill(rax);

    jmp(&done);

    bind(&nil_result);
    movq(rax, slot().base());

    bind(&done);

  } else if (op->subtype() == UnOp::kPlus || op->subtype() == UnOp::kMinus) {
    // +a = 0 + a
    // -a = 0 - a
    // TODO: Parser should genereate negative numbers where possible

    AstNode* zero = new AstNode(AstNode::kNumber);
    zero->value("0");
    zero->length(1);

    AstNode* wrap = new BinOp(
        op->subtype() == UnOp::kPlus ? BinOp::kAdd : BinOp::kSub,
        zero,
        op->lhs());

    VisitForValue(wrap);

  } else if (op->subtype() == UnOp::kNot) {
    // Get value and convert it to boolean
    VisitForValue(op->lhs());
    Call(stubs()->GetCoerceToBooleanStub());

    Label done(this), ret_false(this);

    Operand truev(root_reg, HContext::GetIndexDisp(Heap::kRootTrueIndex));
    Operand falsev(root_reg, HContext::GetIndexDisp(Heap::kRootFalseIndex));

    // Negate it
    IsTrue(rax, NULL, &ret_false);

    movq(rax, truev);

    jmp(&done);
    bind(&ret_false);

    movq(rax, falsev);

    bind(&done);

  } else {
    UNEXPECTED
  }

  return node;
}


AstNode* Fullgen::VisitBinOp(AstNode* node) {
  BinOp* op = BinOp::Cast(node);

  if (visiting_for_slot()) {
    Throw(Heap::kErrorIncorrectLhs);
    return node;
  }

  VisitForValue(op->lhs());

  Label call_stub(this), done(this);

  AstNode* rhs = op->rhs();

  // Fast case : unboxed +/- const
  if (rhs->is(AstNode::kNumber) &&
      !StringIsDouble(rhs->value(), rhs->length()) &&
      (op->subtype() == BinOp::kAdd || op->subtype() == BinOp::kSub)) {

    IsUnboxed(rax, &call_stub, NULL);

    int64_t num = TagNumber(StringToInt(rhs->value(), rhs->length()));

    // addq and subq supports only long immediate (not quad)
    if (num >= -0x7fffffff && num <= 0x7fffffff) {
      switch (op->subtype()) {
       case BinOp::kAdd: addq(rax, Immediate(num)); break;
       case BinOp::kSub: subq(rax, Immediate(num)); break;
       default:
        UNEXPECTED
        break;
      }

      jmp(kNoOverflow, &done);

      // Restore on overflow
      switch (op->subtype()) {
       case BinOp::kAdd: subq(rax, Immediate(num)); break;
       case BinOp::kSub: addq(rax, Immediate(num)); break;
       default:
        UNEXPECTED
        break;
      }
    }
  }

  char* stub = NULL;

#define BINARY_SUB_TYPES(V)\
    V(Add)\
    V(Sub)\
    V(Mul)\
    V(Div)\
    V(Mod)\
    V(BAnd)\
    V(BOr)\
    V(BXor)\
    V(Shl)\
    V(Shr)\
    V(UShr)\
    V(Eq)\
    V(StrictEq)\
    V(Ne)\
    V(StrictNe)\
    V(Lt)\
    V(Gt)\
    V(Le)\
    V(Ge)\
    V(LOr)\
    V(LAnd)

#define BINARY_SUB_ENUM(V)\
    case BinOp::k##V: stub = stubs()->GetBinary##V##Stub(); break;


  switch (op->subtype()) {
   BINARY_SUB_TYPES(BINARY_SUB_ENUM)
   default: emitb(0xcc); break;
  }
#undef BINARY_SUB_ENUM
#undef BINARY_SUB_TYPES

  assert(stub != NULL);

  bind(&call_stub);

  Spill rax_s(this, rax);
  VisitForValue(op->rhs());
  movq(rbx, rax);
  rax_s.Unspill(rax);

  Call(stub);

  bind(&done);

  return node;
}

} // namespace internal
} // namespace candor
