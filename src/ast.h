#ifndef _SRC_AST_H_
#define _SRC_AST_H_

#include "zone.h" // ZoneObject
#include "utils.h" // List
#include "lexer.h" // lexer
#include "scope.h" // Scope

namespace dotlang {

// Forward declaration
struct ScopeSlot;
class AstNode;
class AstValue;

// Just to simplify future use cases
typedef List<AstNode*, ZoneObject> AstList;

#define TYPE_MAPPING_NORMAL(V)\
    V(kBlock)\
    V(kBlockExpr)\
    V(kScopeDecl)\
    V(kMember)\
    V(kValue)\
    V(kMValue)\
    V(kProperty)\
    V(kAssign)\
    V(kIf)\
    V(kWhile)\
    V(kBreak)\
    V(kReturn)\
    V(kFunction)\
    V(kPreInc)\
    V(kPreDec)\
    V(kNot)\
    V(kPostInc)\
    V(kPostDec)

#define TYPE_MAPPING_LEXER(V)\
    V(kName)\
    V(kNumber)\
    V(kString)\
    V(kTrue)\
    V(kFalse)\
    V(kNil)\
    V(kAdd)\
    V(kSub)\
    V(kDiv)\
    V(kMul)\
    V(kBAnd)\
    V(kBOr)\
    V(kBXor)\
    V(kEq)\
    V(kStrictEq)\
    V(kNe)\
    V(kStrictNe)\
    V(kLt)\
    V(kGt)\
    V(kLe)\
    V(kGe)\
    V(kLOr)\
    V(kLAnd)

// Base class
class AstNode : public ZoneObject {
 public:
  enum Type {

#define MAP_DF(x) x,
    TYPE_MAPPING_NORMAL(MAP_DF)
    TYPE_MAPPING_LEXER(MAP_DF)
#undef MAP_DF

    kNop
  };

  AstNode(Type type) : type_(type),
                       value_(NULL),
                       length_(0),
                       stack_count_(0),
                       context_count_(0) {
  }

  virtual ~AstNode() {
  }

  // Converts lexer's token type to ast node type if possible
  inline static Type ConvertType(Lexer::TokenType type) {
    switch (type) {
#define MAP_DF(x) case Lexer::x: return x;
      TYPE_MAPPING_LEXER(MAP_DF)
#undef MAP_DF
     default:
      return kNop;
    }
  }

  // Loads token value and length into ast node
  inline AstNode* FromToken(Lexer::Token* token) {
    value_ = token->value_;
    length_ = token->length_;

    return this;
  }

  // Some shortcuts
  inline AstList* children() { return &children_; }
  inline AstNode* lhs() { return children()->head()->value(); }
  inline AstNode* rhs() { return children()->head()->next()->value(); }

  inline Type type() { return type_; }
  inline bool is(Type type) { return type_ == type; }
  inline int32_t stack_slots() { return stack_count_; }
  inline int32_t context_slots() { return context_count_; }

  // Some node (such as Functions) have context and stack variables
  // SetScope will save that information for future uses in generation
  inline void SetScope(Scope* scope) {
    stack_count_ = scope->stack_count();
    context_count_ = scope->context_count();
  }

  bool PrintChildren(PrintBuffer* p, AstList* children) {
    AstList::Item* item = children->head();
    while (item != NULL) {
      if (!item->value()->Print(p)) return false;
      item = item->next();
      if (item != NULL && !p->Print(" ")) return false;
    }

    return true;
  }

  virtual bool Print(PrintBuffer* p) {
    const char* strtype;
    switch (type()) {
#define MAP_DF(x) case x: strtype = #x; break;
      TYPE_MAPPING_NORMAL(MAP_DF)
      TYPE_MAPPING_LEXER(MAP_DF)
#undef MAP_DF
     default:
      strtype = "kNop";
      break;
    }

    return (is(kName) || is(kTrue) || is (kFalse) || is(kNumber) || is(kNil) ?
               p->Print("[")
               :
               p->Print("[%s ", strtype)
           ) &&

           (length_ == 0 ||
           p->PrintValue(value_, length_) &&
           (children()->length() == 0 || p->Print(" "))) &&

           PrintChildren(p, children()) &&
           p->Print("]");
  }

  Type type_;

  const char* value_;
  uint32_t length_;

  int32_t stack_count_;
  int32_t context_count_;

  AstList children_;
};
#undef TYPE_MAPPING_NORMAL
#undef TYPE_MAPPING_LEXER


// Specific AST node for function,
// contains name and variables list
class FunctionLiteral : public AstNode {
 public:
  FunctionLiteral(AstNode* variable, uint32_t offset) : AstNode(kFunction) {
    variable_ = variable;

    offset_ = offset;
    length_ = 0;
  }

  static inline FunctionLiteral* Cast(AstNode* node) {
    return reinterpret_cast<FunctionLiteral*>(node);
  }

  inline bool CheckDeclaration() {
    // Function without body is a call
    if (children()->length() == 0) {
      // So it should have a name
      if (variable_ == NULL) return false;
      return true;
    }

    // Name should not be "a.b.c"
    if (variable_ != NULL && !variable_->is(kName)) return false;

    // Arguments should be a kName, not expressions
    AstList::Item* head;
    for (head = args_.head(); head != NULL; head = head->next()) {
      if (!head->value()->is(kName)) return false;
    }

    return true;
  }

  // Function literal will keep it's boundaries in original source
  inline FunctionLiteral* End(uint32_t end) {
    length_ = end - offset_;
    return this;
  }


  bool Print(PrintBuffer* p) {
    return p->Print("[kFunction ") &&
           (variable_ == NULL ?
               p->Print("(anonymous)")
               :
               variable_->Print(p)
           ) &&
           p->Print(" @[") &&
           PrintChildren(p, args()) &&
           p->Print("] ") &&
           PrintChildren(p, children()) &&
           p->Print("]");
  }

  inline AstList* args() { return &args_; }

  AstNode* variable_;
  AstList args_;

  uint32_t offset_;
  uint32_t length_;
};


// Every kName AST node will be replaced by
// AST value with scope information
// (is variable on-stack or in-context, it's index, and etc)
class AstValue : public AstNode {
 public:
  AstValue(Scope* scope, AstNode* name) : AstNode(kValue) {
    slot_ = scope->GetSlot(name->value_, name->length_);
    name_ = name;
  }

  static inline AstValue* Cast(AstNode* node) {
    return reinterpret_cast<AstValue*>(node);
  }

  inline ScopeSlot* slot() { return slot_; }
  inline AstNode* name() { return name_; }

 protected:
  ScopeSlot* slot_;
  AstNode* name_;
};

} // namespace dotlang

#endif // _SRC_AST_H_
