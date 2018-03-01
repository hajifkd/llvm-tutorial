#include <iostream>
#include <map>
#include <memory>
#include <stdio.h>
#include <string>
#include <vector>

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

  if (LastChar == '\n') {
    LastChar = ' ';
    return ';';
  }

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
};

class NumberExprAST : public ExprAST {
  double Val;

public:
  NumberExprAST(double Val) : Val(Val) {}
};

class VariableExprAST : public ExprAST {
  std::string Name;

public:
  VariableExprAST(const std::string &Name) : Name(Name) {}
};

class BinaryExprAST : public ExprAST {
  char Op;
  std::unique_ptr<ExprAST> LHS, RHS;

public:
  BinaryExprAST(char op, std::unique_ptr<ExprAST> LHS,
                std::unique_ptr<ExprAST> RHS)
      : Op(op), LHS(std::move(LHS)), RHS(std::move(RHS)) {}
};

class CallExprAST : public ExprAST {
  std::string Callee;
  std::vector<std::unique_ptr<ExprAST>> Args;

public:
  CallExprAST(const std::string &Callee,
              std::vector<std::unique_ptr<ExprAST>> Args)
      : Callee(Callee), Args(std::move(Args)) {}
};

class PrototypeAST {
  std::string Name;
  std::vector<std::string> Args;

public:
  PrototypeAST(const std::string &name, std::vector<std::string> Args)
      : Name(name), Args(std::move(Args)) {}

  const std::string &getName() const { return Name; }
};

class FunctionAST {
  std::unique_ptr<PrototypeAST> Proto;
  std::unique_ptr<ExprAST> Body;

public:
  FunctionAST(std::unique_ptr<PrototypeAST> Proto,
              std::unique_ptr<ExprAST> Body)
      : Proto(std::move(Proto)), Body(std::move(Body)) {}
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
      if (CurTok != '.')
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
    auto Proto = std::make_unique<PrototypeAST>("", std::vector<std::string>());

    return std::make_unique<FunctionAST>(std::move(Proto), std::move(E));
  }

  return nullptr;
}

// handlers

static void HandleDefinition() {
  if (ParseDefinition()) {
    fprintf(stderr, "Parsed a function definition.\n");
  } else {
    getNextToken();
  }
}

static void HandleExtern() {
  if (ParseExtern()) {
    fprintf(stderr, "Parsed an extern\n");
  } else {
    getNextToken();
  }
}

static void HandleTopLevelExpression() {
  if (ParseTopLevelExpr()) {
    fprintf(stderr, "Parsed a top-level expr\n");
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
  BinopPrecedence['<'] = 10;
  BinopPrecedence['+'] = 20;
  BinopPrecedence['-'] = 20;
  BinopPrecedence['*'] = 40;

  getNextToken();
  MainLoop();

  return 0;
}