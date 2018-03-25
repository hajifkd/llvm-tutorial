#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/GVN.h"

#include "./include/KaleidoscopeJIT.h"

#include <iostream>
#include <map>
#include <memory>
#include <stdio.h>
#include <string>
#include <vector>

using namespace llvm;

// Lexer

enum Token {
  tok_eof = -1,

  tok_def = -2,
  tok_extern = -3,

  tok_identifier = -4,
  tok_number = -5,
};

static std::string IdentifierStr;
static double NumVal;

static char getBufChar() {
  static unsigned int Index = 0;
  static std::string Buf;

  if (Buf.size() <= Index) {
    fprintf(stderr, "ready > ");
    std::getline(std::cin, Buf);
    Buf += '\n';
    Index = 0;
  }

  return Buf.c_str()[Index++];
}

static int gettok() {
  static int LastChar = ' ';

  while (isspace(LastChar))
    LastChar = getBufChar();

  if (isalpha(LastChar)) {
    IdentifierStr = LastChar;

    while (isalnum((LastChar = getBufChar())))
      IdentifierStr += LastChar;

    if (IdentifierStr == "def")
      return tok_def;

    if (IdentifierStr == "extern")
      return tok_extern;

    return tok_identifier;
  }

  if (isdigit(LastChar) || LastChar == '.') {
    std::string NumStr;

    do {
      NumStr += LastChar;
      LastChar = getBufChar();
    } while (isdigit(LastChar) || LastChar == '.');

    NumVal = strtod(NumStr.c_str(), 0);

    return tok_number;
  }

  if (LastChar == '#') {
    do
      LastChar = getBufChar();
    while (LastChar != EOF && LastChar != '\n' && LastChar != '\r');

    if (LastChar != EOF)
      return gettok();
  }

  if (LastChar == EOF)
    return tok_eof;

  int ThisChar = LastChar;
  LastChar = getBufChar();

  return ThisChar;
}

// AST

class ExprAST {
public:
  virtual ~ExprAST() {}
  virtual Value *codegen() = 0;
};

class NumberExprAST : public ExprAST {
  double Val;

public:
  NumberExprAST(double Val) : Val(Val) {}
  virtual Value *codegen() override;
};

class VariableExprAST : public ExprAST {
  std::string Name;

public:
  VariableExprAST(const std::string &Name) : Name(Name) {}
  virtual Value *codegen() override;
};

class BinaryExprAST : public ExprAST {
  char Op;
  std::unique_ptr<ExprAST> LHS, RHS;

public:
  BinaryExprAST(char op, std::unique_ptr<ExprAST> LHS,
                std::unique_ptr<ExprAST> RHS)
      : Op(op), LHS(std::move(LHS)), RHS(std::move(RHS)) {}
  virtual Value *codegen() override;
};

class CallExprAST : public ExprAST {
  std::string Callee;
  std::vector<std::unique_ptr<ExprAST>> Args;

public:
  CallExprAST(const std::string &Callee,
              std::vector<std::unique_ptr<ExprAST>> Args)
      : Callee(Callee), Args(std::move(Args)) {}
  virtual Value *codegen() override;
};

class PrototypeAST {
  std::string Name;
  std::vector<std::string> Args;

public:
  PrototypeAST(const std::string &name, std::vector<std::string> Args)
      : Name(name), Args(std::move(Args)) {}

  const std::string &getName() const { return Name; }
  Function *codegen();
};

class FunctionAST {
  std::unique_ptr<PrototypeAST> Proto;
  std::unique_ptr<ExprAST> Body;

public:
  FunctionAST(std::unique_ptr<PrototypeAST> Proto,
              std::unique_ptr<ExprAST> Body)
      : Proto(std::move(Proto)), Body(std::move(Body)) {}
  Function *codegen();
};

// Parser

static std::unique_ptr<ExprAST> ParseExpression();

static int CurTok;
static int getNextToken() { return CurTok = gettok(); }

std::unique_ptr<ExprAST> LogError(const char *Str) {
  fprintf(stderr, "LogError: %s\n", Str);
  return nullptr;
}

std::unique_ptr<PrototypeAST> LogErrorP(const char *Str) {
  LogError(Str);
  return nullptr;
}

// numberexpr ::= number
static std::unique_ptr<ExprAST> ParseNumberExpr() {
  // Now we have make_unique in C++14
  auto Result = std::make_unique<NumberExprAST>(NumVal);
  getNextToken();
  return std::move(Result);
}

// parenexpr ::= '(' expr ')'
static std::unique_ptr<ExprAST> ParseParenExpr() {
  getNextToken(); // consume '(' (This '(' is assumed.)

  auto V = ParseExpression();

  if (!V)
    return nullptr;

  if (CurTok != ')')
    return LogError("expected ')'");

  getNextToken();
  return V;
}

// identifierexpr
//    ::= identifier # just a variable
//    ::= identifier '(' expr* ')' # call a function
static std::unique_ptr<ExprAST> ParseIdentifierExpr() {
  std::string IdName = IdentifierStr;

  getNextToken(); // Consume IdName

  if (CurTok != '(') // just a variable
    return std::make_unique<VariableExprAST>(IdName);

  // Call
  getNextToken(); // Consume ')'
  std::vector<std::unique_ptr<ExprAST>> Args;

  if (CurTok != ')') {
    while (true) {
      if (auto Arg = ParseExpression())
        Args.push_back(std::move(Arg));
      else
        return nullptr;

      if (CurTok == ')')
        break;
      if (CurTok != ',')
        return LogError("expected ')' or ',' in argument list");

      getNextToken(); // Consume ','
    }
  }
  getNextToken(); // Consume ')'

  return std::make_unique<CallExprAST>(IdName, std::move(Args));
}

// primary
//    ::= identifierexpr
//    ::= numberexpr
//    ::= parenexpr
static std::unique_ptr<ExprAST> ParsePrimary() {
  switch (CurTok) {
  case tok_identifier:
    return ParseIdentifierExpr();
  case tok_number:
    return ParseNumberExpr();
  case '(':
    return ParseParenExpr();
  default:
    return LogError("unknown token");
  }
}

// Priorities of binary operators
static std::map<char, int> BinopPrecedence;

static int GetTokPrecedence() {
  if (!isascii(CurTok))
    return -1;

  int TokPrec = BinopPrecedence[CurTok];
  if (TokPrec <= 0)
    return -1;
  return TokPrec;
}

static std::unique_ptr<ExprAST> ParseBinOpRHS(int, std::unique_ptr<ExprAST>);

// expression
//    ::= primary binopRHS
static std::unique_ptr<ExprAST> ParseExpression() {
  auto LHS = ParsePrimary();
  if (!LHS)
    return nullptr;

  return ParseBinOpRHS(0, std::move(LHS));
}

// binopRHS
//    ::= (op primery)*
static std::unique_ptr<ExprAST> ParseBinOpRHS(int ExprPrec,
                                              std::unique_ptr<ExprAST> LHS) {
  while (1) {
    int TokPrec = GetTokPrecedence();

    if (TokPrec < ExprPrec)
      return LHS; // just return

    int BinOp = CurTok;
    getNextToken();

    auto RHS = ParsePrimary();
    if (!RHS)
      return nullptr;

    // decide association law
    int NextPrec = GetTokPrecedence();
    if (TokPrec < NextPrec) {
      RHS = ParseBinOpRHS(TokPrec, std::move(RHS));
      if (!RHS)
        return nullptr;
    }

    LHS =
        std::make_unique<BinaryExprAST>(BinOp, std::move(LHS), std::move(RHS));
  }
}

static std::unique_ptr<PrototypeAST> ParsePrototype() {
  if (CurTok != tok_identifier)
    return LogErrorP("expected function name");

  std::string FnName = IdentifierStr;
  getNextToken();

  if (CurTok != '(')
    return LogErrorP("expected '('");

  std::vector<std::string> ArgNames;
  while (getNextToken() == tok_identifier)
    ArgNames.push_back(IdentifierStr);

  if (CurTok != ')')
    return LogErrorP("expected ')'");

  getNextToken();

  return std::make_unique<PrototypeAST>(FnName, std::move(ArgNames));
}

static std::unique_ptr<FunctionAST> ParseDefinition() {
  getNextToken(); // consume def
  auto Proto = ParsePrototype();
  if (!Proto)
    return nullptr;

  if (auto E = ParseExpression())
    return std::make_unique<FunctionAST>(std::move(Proto), std::move(E));

  return nullptr;
}

static std::unique_ptr<PrototypeAST> ParseExtern() {
  getNextToken();
  return ParsePrototype();
}

static std::unique_ptr<FunctionAST> ParseTopLevelExpr() {
  if (auto E = ParseExpression()) {
    // anon fn
    auto Proto = std::make_unique<PrototypeAST>("__anon_expr",
                                                std::vector<std::string>());

    return std::make_unique<FunctionAST>(std::move(Proto), std::move(E));
  }

  return nullptr;
}

// code generation

// holding llvm structures
static LLVMContext TheContext;
// generate llvm instructions
static IRBuilder<> Builder(TheContext);
// llvm structure to hold functions and global variables
// own whole `Value`s
static std::unique_ptr<Module> TheModule;
// pass manager - an optimizer
static std::unique_ptr<legacy::FunctionPassManager> TheFPM;
// keeps track of which values are defined in the given context
// and their repr in LLVM IR
static std::map<std::string, Value *> NamedValues;
// JIT
static std::unique_ptr<orc::KaleidoscopeJIT> TheJIT;

static std::map<std::string, std::unique_ptr<PrototypeAST>> FunctionProtos;

void InitializeModuleAndPassManager() {
  TheModule = std::make_unique<Module>("my cool jit", TheContext);
  TheModule->setDataLayout(TheJIT->getTargetMachine().createDataLayout());

  TheFPM = std::make_unique<legacy::FunctionPassManager>(TheModule.get());

  // "peephole" - bit manipulation?
  TheFPM->add(createInstructionCombiningPass());

  // Reassociation
  TheFPM->add(createReassociatePass());

  // eliminate common subexpressions
  TheFPM->add(createGVNPass());

  // Simplify control flow graphs
  TheFPM->add(createCFGSimplificationPass());

  // See llvm::legacy::FunctionPassManager instead?
  TheFPM->doInitialization();
}

Function *getFunction(std::string Name) {
  if (auto *F = TheModule->getFunction(Name))
    return F;

  auto FI = FunctionProtos.find(Name);
  if (FI != FunctionProtos.end())
    return FI->second->codegen();

  return nullptr;
}

Value *LogErrorV(const char *Str) {
  LogError(Str);
  return nullptr;
}

Value *NumberExprAST::codegen() {
  return ConstantFP::get(TheContext, APFloat(Val));
}

Value *VariableExprAST::codegen() {
  Value *V = NamedValues[Name];
  if (!V)
    return LogErrorV("Unknown variable name");
  else
    return V;
}

Value *BinaryExprAST::codegen() {
  Value *L = LHS->codegen();
  Value *R = RHS->codegen();

  if (!L || !R)
    return nullptr;

  switch (Op) {
  case '+':
    // third arg is just for convenience - essentially unnecessary
    return Builder.CreateFAdd(L, R, "addtmp");
  case '-':
    return Builder.CreateFSub(L, R, "subtmp");
  case '*':
    return Builder.CreateFMul(L, R, "multmp");
  case '<':
    L = Builder.CreateFCmpULT(L, R, "cmptmp");
    return Builder.CreateUIToFP(L, Type::getDoubleTy(TheContext), "booltmp");
  default:
    return LogErrorV("invalid binary operator");
  }
}

Value *CallExprAST::codegen() {
  // Look up the name
  Function *CalleeF = getFunction(Callee);
  if (!CalleeF)
    return LogErrorV("Unknown function called");

  if (CalleeF->arg_size() != Args.size())
    return LogErrorV("invalid number of arguments passed");

  std::vector<Value *> ArgsV;
  for (auto &Arg : Args) {
    ArgsV.push_back(Arg->codegen());

    if (!ArgsV.back())
      return nullptr;
  }

  return Builder.CreateCall(CalleeF, ArgsV, "calltmp");
}

Function *PrototypeAST::codegen() {
  std::vector<Type *> Doubles(Args.size(), Type::getDoubleTy(TheContext));
  FunctionType *FT =
      FunctionType::get(Type::getDoubleTy(TheContext), Doubles, false);
  Function *F =
      Function::Create(FT, Function::ExternalLinkage, Name, TheModule.get());

  // registering names for args
  unsigned int Idx = 0;
  for (auto &Arg : F->args())
    Arg.setName(Args[Idx++]);

  return F;
}

Function *FunctionAST::codegen() {
  auto &P = *Proto;
  FunctionProtos[Proto->getName()] = std::move(Proto);
  Function *TheFunction = getFunction(P.getName());

  if (!TheFunction)
    return nullptr;

  if (!TheFunction->empty())
    return (Function *)LogErrorV("Function cannot be redefined");

  BasicBlock *BB = BasicBlock::Create(TheContext, "entry", TheFunction);
  Builder.SetInsertPoint(BB);

  NamedValues.clear();
  for (auto &Arg : TheFunction->args())
    NamedValues[Arg.getName()] = &Arg;

  if (Value *RetVal = Body->codegen()) {
    Builder.CreateRet(RetVal);

    verifyFunction(*TheFunction);

    TheFPM->run(*TheFunction);

    return TheFunction;
  }

  TheFunction->eraseFromParent();
  return nullptr;
}

// handlers

static void HandleDefinition() {
  if (auto FnAST = ParseDefinition()) {
    if (auto *FnIR = FnAST->codegen()) {
      fprintf(stderr, "Read function definition:");
      FnIR->print(errs());
      fprintf(stderr, "\n");
      TheJIT->addModule(std::move(TheModule));
      InitializeModuleAndPassManager();
    }
  } else {
    getNextToken();
  }
}

static void HandleExtern() {
  if (auto ProtoAST = ParseExtern()) {
    if (auto *FnIR = ProtoAST->codegen()) {
      fprintf(stderr, "Read extern: ");
      FnIR->print(errs());
      fprintf(stderr, "\n");
      FunctionProtos[ProtoAST->getName()] = std::move(ProtoAST);
    }
  } else {
    getNextToken();
  }
}

static void HandleTopLevelExpression() {
  if (auto FnAST = ParseTopLevelExpr()) {
    if (auto *FnIR = FnAST->codegen()) {
      fprintf(stderr, "Read top-level expression: ");
      FnIR->print(errs());

      auto H = TheJIT->addModule(std::move(TheModule));
      InitializeModuleAndPassManager();

      auto ExprSymbol = TheJIT->findSymbol("__anon_expr");
      assert(ExprSymbol && "Function not found");

      double (*FP)() = (double (*)())(intptr_t)ExprSymbol.getAddress();
      fprintf(stderr, "Evaluated to %f\n", FP());

      TheJIT->removeModule(H);
    }
  } else {
    getNextToken();
  }
}

static void MainLoop() {
  while (1) {
    switch (CurTok) {
    case tok_eof:
      return;

    case ';':
      getNextToken();
      break;

    case tok_def:
      HandleDefinition();
      break;

    case tok_extern:
      HandleExtern();
      break;

    default:
      HandleTopLevelExpression();
      break;
    }
  }
}

int main() {
  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();
  InitializeNativeTargetAsmParser();

  BinopPrecedence['<'] = 10;
  BinopPrecedence['+'] = 20;
  BinopPrecedence['-'] = 20;
  BinopPrecedence['*'] = 40;

  getNextToken();

  TheJIT = std::make_unique<orc::KaleidoscopeJIT>();

  InitializeModuleAndPassManager();
  MainLoop();

  TheModule->print(errs(), nullptr);

  return 0;
}