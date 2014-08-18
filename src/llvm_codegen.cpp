#include "llvm_codegen.h"

#include <iostream>
#include <stack>

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/Type.h"

#include "llvm/Analysis/Verifier.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/ExecutionEngine/JIT.h"

#include "irvisitors.h"
#include "macros.h"
#include "ir.h"
#include "tensor.h"
#include "symboltable.h"

using namespace std;

#define LLVM_CONTEXT   llvm::getGlobalContext()

#define LLVM_VOID      llvm::Type::getVoidTy(LLVM_CONTEXT)
#define LLVM_INT       llvm::Type::getInt32Ty(LLVM_CONTEXT)
#define LLVM_INTPTR    llvm::Type::getInt32PtrTy(LLVM_CONTEXT)
#define LLVM_DOUBLE    llvm::Type::getDoubleTy(LLVM_CONTEXT)
#define LLVM_DOUBLEPTR llvm::Type::getDoublePtrTy(LLVM_CONTEXT)

namespace {

llvm::Type *toLLVMType(const simit::internal::TensorType *type) {
  llvm::Type *llvmType = NULL;
  if (type->getOrder() == 0) {
    switch (type->getComponentType()) {
      case simit::Type::INT:
        llvmType = LLVM_INTPTR;
        break;
      case simit::Type::FLOAT:
        llvmType = LLVM_DOUBLEPTR;
        break;
      case simit::Type::ELEMENT:
        return NULL;  // TODO: not supported yet
        break;
      default:
        UNREACHABLE_DEFAULT;
    }
  }
  else {
    return NULL;  // TODO: not supported yet
  }

  assert(llvmType != NULL);
  return llvmType;
}

inline llvm::ConstantInt* constant(int val) {
    return llvm::ConstantInt::get(LLVM_CONTEXT, llvm::APInt(32, val, false));
}

typedef simit::internal::IndexExpr::IndexedTensor IndexedTensor;
typedef void (*FuncPtr)(...);

}  // unnamed namespace


namespace simit {
namespace internal {

class LLVMBinaryFunction {
 public:

  LLVMBinaryFunction(const FuncPtr &fptr) : fptr{fptr} {}

  void run() {
    UNUSED(fptr);
  }

 private:
  FuncPtr fptr;
};

class LLVMCodeGenImpl : public IRVisitor {
 public:
  LLVMCodeGenImpl() : builder(LLVM_CONTEXT) {
    if (!llvmInitialized) {
      llvm::InitializeNativeTarget();
      llvmInitialized = true;
    }

    module = new llvm::Module("Simit JIT", LLVM_CONTEXT);
    llvm::EngineBuilder engineBuilder(module);
    executionEngine = engineBuilder.create();
  }

  BinaryFunction *compileToFunctionPointer(Function *function);
  llvm::Function *codegen(Function *function);

 private:
  llvm::Module          *module;
  llvm::ExecutionEngine *executionEngine;
  llvm::IRBuilder<>      builder;
  static bool llvmInitialized;

  SymbolTable<llvm::Value*> symtable;
  std::stack<llvm::Value*>  resultStack;

  void handle(Function *function);
  void handle(Literal *t);
  void handle(IndexExpr     *t);
  void handle(VariableStore *t);

  llvm::Function *createFunctionPrototype(Function *function);
  llvm::Value *createScalarOp(const std::string &name, IndexExpr::Operator op,
                              const vector<IndexedTensor> &operands);
};
bool LLVMCodeGenImpl::llvmInitialized = false;

BinaryFunction *LLVMCodeGenImpl::compileToFunctionPointer(Function *function) {
  llvm::Function *f = codegen(function);
  if (f == NULL) return NULL;
  f->dump();

  FuncPtr fptr = (FuncPtr)executionEngine->getPointerToFunction(f);
  LLVMBinaryFunction *binFunc = new LLVMBinaryFunction(fptr);
  UNUSED(binFunc);

  double input = 1.0;
  double output = 0.0;
  fptr(&input, &output);

  cout << input << "->" << output << endl;

  // Pack up the llvm::Function in a simit BinaryFunction object
  return NULL;
}

llvm::Function *LLVMCodeGenImpl::codegen(Function *function) {
  visit(function);
  if (isAborted()) {
    return NULL;
  }
  builder.CreateRetVoid();

  llvm::Value *value = resultStack.top();
  resultStack.pop();
  assert(llvm::isa<llvm::Function>(value));
  llvm::Function *f = llvm::cast<llvm::Function>(value);
  verifyFunction(*f);
  return f;
}

void LLVMCodeGenImpl::handle(Literal *t) {
  cout << "Literal:   " << *t << endl;
}

void LLVMCodeGenImpl::handle(IndexExpr *t) {
  cout << "IndexExpr: " << *t << endl;
  llvm::Value *result = NULL;

  auto domain = t->getDomain();
  auto op = t->getOperator();
  auto &operands = t->getOperands();

  if (domain.size() == 0) {
    result = createScalarOp(t->getName(), op, operands);
  }
  else {
    NOT_SUPPORTED_YET;
  }

  assert(result != NULL);
  symtable.insert(t->getName(), result);
}

void LLVMCodeGenImpl::handle(VariableStore *t) {
  cout << "Store   :  " << *t << endl;

  assert(symtable.contains(t->getValue()->getName()));
  auto val = symtable.get(t->getValue()->getName());

  assert(symtable.contains(t->getTarget()->getName()));
  auto target = symtable.get(t->getTarget()->getName());

  builder.CreateStore(val, target);
}

llvm::Function *LLVMCodeGenImpl::createFunctionPrototype(Function *function) {
  vector<llvm::Type*> args;
  for (auto &arg : function->getArguments()) {
    auto llvmType = toLLVMType(arg->getType());
    if (llvmType == NULL) return NULL;  // TODO: Remove check
    args.push_back(llvmType);
  }
  for (auto &result : function->getResults()) {
    auto llvmType = toLLVMType(result->getType());
    if (llvmType == NULL) return NULL;  // TODO: Remove check
    args.push_back(llvmType);
  }
  llvm::FunctionType *ft = llvm::FunctionType::get(LLVM_VOID, args, false);
  llvm::Function *f = llvm::Function::Create(ft,
                                             llvm::Function::ExternalLinkage,
                                             function->getName(),
                                             module);
  // Name arguments and results
  auto ai = f->arg_begin();
  for (auto &arg : function->getArguments()) {
    ai->setName(arg->getName());
    symtable.insert(arg->getName(), ai);
    ++ai;
  }
  for (auto &result : function->getResults()) {
    ai->setName(result->getName());
    symtable.insert(result->getName(), ai);
    ++ai;
  }
  assert(ai == f->arg_end());
  return f;
}

void LLVMCodeGenImpl::handle(Function *function) {
  llvm::Function *f = createFunctionPrototype(function);
  if (f == NULL) {  // TODO: Remove check
    abort();
    return;
  }
  auto entry = llvm::BasicBlock::Create(LLVM_CONTEXT, "entry", f);
  builder.SetInsertPoint(entry);

  resultStack.push(f);
}

llvm::Value *
LLVMCodeGenImpl::createScalarOp(const std::string &name, IndexExpr::Operator op,
                                const vector<IndexedTensor> &operands) {
  llvm::Value *value = NULL;
  switch (op) {
    case IndexExpr::NEG: {
      assert (operands.size() == 1);
      auto operandName = operands[0].getTensor()->getName();
      assert(symtable.contains(operandName));
      auto val = symtable.get(operandName);
      auto type = val->getType();
      if (type->isPointerTy()) {
        auto addr = builder.CreateGEP(val, constant(0), operandName + "_ptr");
        val = builder.CreateLoad(addr, operandName + "_val");
      }
      value = builder.CreateFNeg(val, name);
      break;
    }
    case IndexExpr::ADD:
      NOT_SUPPORTED_YET;
      break;
    case IndexExpr::SUB:
      NOT_SUPPORTED_YET;
      break;
    case IndexExpr::MUL:
      NOT_SUPPORTED_YET;
      break;
    case IndexExpr::DIV:
      NOT_SUPPORTED_YET;
      break;
  }

  assert(value != NULL);
  return value;
}


// class LLVMCodeGen
LLVMCodeGen::LLVMCodeGen() : impl(new LLVMCodeGenImpl()) {
}

LLVMCodeGen::~LLVMCodeGen() {
  delete impl;
}

BinaryFunction *LLVMCodeGen::compileToFunctionPointer(Function *function) {
  cout << *function << endl;
  return impl->compileToFunctionPointer(function);
}

}}  // namespace simit::internal