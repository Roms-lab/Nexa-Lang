#pragma once

#include "Lexer.hpp"
#include "Modules.hpp"
#include <string>
#include <vector>
#include <set>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <stdexcept>

namespace nexa {

// AST node types - simple structure for our minimal grammar
struct AstNode {
    enum class Type { Include, IoPrint, IoPrintln, IoReadln, IoToInt, MainFunction, Function, FnCall, Variable, Assignment, OsSystem, OsGetenv, OsPlatform, OsExeDir,
                      DllLoad, DllCall,
                      FileRead, FileWrite, FileAppend, FileExists,
                      RandomInt, RandomSeed,
                      TimeSleep, TimeSeconds, TimeMilliseconds,
                      IfElse,
                      Switch,
                      SwitchCase,
                      While,
                      For,
                      Return,
                      Break,
                      Continue,
                      IncPost,
                      DecPost,
                      AssnAdd,
                      AssnSub,
                      AssnMul,
                      AssnDiv,
                      AssnMod,
                      Block,
                      ExprIntLiteral, ExprFloatLiteral, ExprCharLiteral, ExprBoolLiteral, ExprVarRef, ExprAdd, ExprSub, ExprMul, ExprDiv, ExprMod,
                      ExprArrayLiteral, ExprArrayIndex,
                      CondEq, CondNe, CondLt, CondGt, CondLe, CondGe,
                      CondAnd, CondOr, CondNot,
                      ExprStringLiteral,
                      AssnIndex };
    Type type;
    std::string value;           // for Include path, string literal, or variable name
    std::vector<AstNode> children;
    std::vector<std::string> paramNames;   // for Function: parameter names
    std::vector<std::string> paramTypes;   // for Function: "int", "string", or "" (default int)
    std::string initValue;       // for Variable: literal initializer value
    bool initIsInt = false;     // for Variable: true = int, false = string
    bool initFromReadln = false; // for Variable: true = io.readln()
    bool initFromDllLoad = false; // for Variable: true = dll.load("path")
    bool initFromArray = false;   // for Variable: true = array literal
    bool initFromFileRead = false; // for Variable: true = file.read()
    bool initUninitialized = false; // for Variable: true = let x; (defaults to int 0)
    bool initIsBool = false;       // for Variable: true = let x = true/false
    bool initIsFloat = false;      // for Variable: true = let x = 3.14
    bool initIsChar = false;       // for Variable: true = let x = 'a'
    std::string declType = "";  // for Variable: "int", "string", "bool", "float", "char", "unsigned char", or "" (inferred)
    bool isVarRef = false;      // for IoPrint/IoPrintln: true = print variable, false = print string
    bool caseIsString = false;  // for SwitchCase: true = case "str", false = case 42
    bool isConst = false;       // for Variable: true = let const x = ...
    bool isFixedArray = false;  // for Variable: true = let x: type[size]; (fixed-size buffer)
    std::string arraySize = ""; // for Variable: size for fixed array, e.g. "4080"
};

class Parser {
public:
    Parser(std::vector<Token> tokens, Modules& modules,
           const std::string& currentFilePath = "",
           std::set<std::string>* includedFiles = nullptr,
           const std::vector<std::string>* packagePaths = nullptr)
        : tokens_(std::move(tokens)), modules_(modules), pos_(0),
          currentFilePath_(currentFilePath), includedFiles_(includedFiles),
          packagePaths_(packagePaths) {}

    std::vector<AstNode> parse() {
        std::vector<AstNode> ast;
        while (pos_ < tokens_.size()) {
            const Token& t = peek();
            if (t.type == TokenType::Eof) break;
            if (t.type == TokenType::Include) {
                std::vector<AstNode> incNodes = parseInclude();
                for (AstNode& n : incNodes) ast.push_back(std::move(n));
            } else if (t.type == TokenType::Fn) {
                if (pos_ + 1 < tokens_.size() && tokens_[pos_ + 1].type == TokenType::Main) {
                    ast.push_back(parseMainFunction());
                } else {
                    ast.push_back(parseFunction());
                }
            } else if (t.type == TokenType::Let) {
                ast.push_back(parseVariable());
            } else {
                throw std::runtime_error("Unexpected token at line " + std::to_string(t.line));
            }
        }
        return ast;
    }

private:
    std::vector<Token> tokens_;
    Modules& modules_;
    size_t pos_;
    std::string currentFilePath_;
    std::set<std::string>* includedFiles_;
    const std::vector<std::string>* packagePaths_;

    static bool exprProducesString(const AstNode& e) {
        if (e.type == AstNode::Type::OsGetenv || e.type == AstNode::Type::ExprStringLiteral || e.type == AstNode::Type::FileRead || e.type == AstNode::Type::IoReadln) return true;
        if (e.type == AstNode::Type::ExprAdd && e.children.size() >= 2) {
            return exprProducesString(e.children[0]) || exprProducesString(e.children[1]);
        }
        return false;
    }

    const Token& peek() const {
        if (pos_ >= tokens_.size()) return tokens_.back();
        return tokens_[pos_];
    }

    const Token& advance() {
        if (pos_ < tokens_.size()) pos_++;
        return tokens_[pos_ - 1];
    }

    bool match(TokenType type) {
        if (pos_ < tokens_.size() && tokens_[pos_].type == type) {
            advance();
            return true;
        }
        return false;
    }

    std::vector<AstNode> parseInclude() {
        const Token& t = advance();
        std::string raw = t.value;
        size_t angleStart = raw.find('<');
        size_t angleEnd = raw.find('>');
        size_t quoteStart = raw.find('"');
        size_t quoteEnd = raw.rfind('"');

        if (angleStart != std::string::npos && angleEnd != std::string::npos && angleEnd > angleStart) {
            std::string path = raw.substr(angleStart + 1, angleEnd - angleStart - 1);
            if (path.size() >= 4 && path.substr(0, 4) == "std/") {
                if (path == "std/ui") {
                    throw std::runtime_error("std/ui has been removed");
                }
                // #include <std/io> - built-in module
                modules_.enable(path);
                return {{AstNode::Type::Include, path, {}}};
            }
            // #include <pkg/module> - package
            std::string pkgPath = path;
            if (pkgPath.size() < 4 || pkgPath.substr(pkgPath.size() - 4) != ".nxa") pkgPath += ".nxa";
            if (packagePaths_) {
                for (const std::string& root : *packagePaths_) {
                    std::filesystem::path full = std::filesystem::path(root) / pkgPath;
                    std::ifstream in(full);
                    if (in) {
                        std::string absPath = std::filesystem::absolute(full).string();
                        if (includedFiles_) {
                            if (includedFiles_->count(absPath)) return {};
                            includedFiles_->insert(absPath);
                        }
                        std::stringstream buf;
                        buf << in.rdbuf();
                        in.close();
                        Lexer lexer(buf.str());
                        std::vector<Token> subTokens = lexer.tokenize();
                        Parser subParser(std::move(subTokens), modules_, absPath, includedFiles_, packagePaths_);
                        return subParser.parse();
                    }
                }
            }
            throw std::runtime_error("Cannot find package: " + path);
        }
        if (quoteStart != std::string::npos && quoteEnd != std::string::npos && quoteEnd > quoteStart) {
            // #include "file.nxa" - file include
            std::string relPath = raw.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
            std::filesystem::path resolved;
            if (!relPath.empty() && (relPath[0] == '/' || (relPath.size() > 1 && relPath[1] == ':'))) {
                resolved = relPath;
            } else {
                std::filesystem::path base = std::filesystem::path(currentFilePath_).parent_path();
                resolved = (base / relPath).lexically_normal();
            }
            std::string absPath = std::filesystem::absolute(resolved).string();
            if (includedFiles_) {
                if (includedFiles_->count(absPath)) return {};  // already included, skip
                includedFiles_->insert(absPath);
            }
            std::ifstream in(absPath);
            if (!in) {
                throw std::runtime_error("Cannot open included file: " + absPath);
            }
            std::stringstream buf;
            buf << in.rdbuf();
            in.close();
            Lexer lexer(buf.str());
            std::vector<Token> subTokens = lexer.tokenize();
            Parser subParser(std::move(subTokens), modules_, absPath, includedFiles_, packagePaths_);
            return subParser.parse();
        }
        throw std::runtime_error("Invalid #include at line " + std::to_string(t.line));
    }

    AstNode parseMainFunction() {
        size_t line = peek().line;
        if (!match(TokenType::Fn)) {
            throw std::runtime_error("Expected 'fn' at line " + std::to_string(line));
        }
        if (!match(TokenType::Main)) {
            throw std::runtime_error("Expected 'main' at line " + std::to_string(line));
        }
        if (!match(TokenType::LParen)) {
            throw std::runtime_error("Expected '(' at line " + std::to_string(line));
        }
        if (!match(TokenType::RParen)) {
            throw std::runtime_error("Expected ')' at line " + std::to_string(line));
        }
        if (!match(TokenType::LBrace)) {
            throw std::runtime_error("Expected '{' at line " + std::to_string(line));
        }

        AstNode mainNode{AstNode::Type::MainFunction, "", {}};
        mainNode.children = parseBlock();
        if (!match(TokenType::RBrace)) {
            throw std::runtime_error("Expected '}' at line " + std::to_string(peek().line));
        }

        return mainNode;
    }

    AstNode parseFunction() {
        size_t line = peek().line;
        if (!match(TokenType::Fn)) {
            throw std::runtime_error("Expected 'fn' at line " + std::to_string(line));
        }
        const Token& nameTok = peek();
        if (nameTok.type != TokenType::Identifier) {
            throw std::runtime_error("Expected function name at line " + std::to_string(nameTok.line));
        }
        advance();
        std::string name = nameTok.value;
        if (name == "main") {
            throw std::runtime_error("Use fn main() for entry point at line " + std::to_string(line));
        }
        if (!match(TokenType::LParen)) {
            throw std::runtime_error("Expected '(' at line " + std::to_string(peek().line));
        }
        std::vector<std::string> params;
        std::vector<std::string> types;
        if (peek().type != TokenType::RParen) {
            for (;;) {
                const Token& p = peek();
                if (p.type != TokenType::Identifier) {
                    throw std::runtime_error("Expected parameter name at line " + std::to_string(p.line));
                }
                params.push_back(p.value);
                advance();
                std::string ptype = "int";  // default
                if (match(TokenType::Colon)) {
                    const Token& t = peek();
                    if (t.type != TokenType::Identifier) {
                        throw std::runtime_error("Expected parameter type (int, string, or bool) at line " + std::to_string(t.line));
                    }
                    if (t.value == "int") ptype = "int";
                    else if (t.value == "string") ptype = "string";
                    else if (t.value == "bool") ptype = "bool";
                    else if (t.value == "float") ptype = "float";
                    else if (t.value == "char") ptype = "char";
                    else throw std::runtime_error("Parameter type must be int, string, bool, float, or char at line " + std::to_string(t.line));
                    advance();
                }
                types.push_back(ptype);
                if (!match(TokenType::Comma)) break;
            }
        }
        if (!match(TokenType::RParen)) {
            throw std::runtime_error("Expected ')' at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::LBrace)) {
            throw std::runtime_error("Expected '{' at line " + std::to_string(peek().line));
        }
        AstNode fnNode{AstNode::Type::Function, name, {}};
        fnNode.paramNames = std::move(params);
        fnNode.paramTypes = std::move(types);
        fnNode.children = parseBlock();
        if (!match(TokenType::RBrace)) {
            throw std::runtime_error("Expected '}' at line " + std::to_string(peek().line));
        }
        return fnNode;
    }

    std::vector<AstNode> parseBlock() {
        std::vector<AstNode> stmts;
        while (pos_ < tokens_.size()) {
            const Token& t = peek();
            if (t.type == TokenType::RBrace) break;
            if (t.type == TokenType::Let) {
                stmts.push_back(parseVariable());
            } else if (t.type == TokenType::If) {
                stmts.push_back(parseIf());
            } else if (t.type == TokenType::Switch) {
                stmts.push_back(parseSwitch());
            } else if (t.type == TokenType::While) {
                stmts.push_back(parseWhile());
            } else if (t.type == TokenType::For) {
                stmts.push_back(parseFor());
            } else if (t.type == TokenType::Identifier && t.value == "io") {
                stmts.push_back(parseIoCall());
            } else if (t.type == TokenType::Identifier && t.value == "os") {
                stmts.push_back(parseOsCall());
            } else if (t.type == TokenType::Identifier && t.value == "dll") {
                stmts.push_back(parseDllCall());
            } else if (t.type == TokenType::Identifier && t.value == "file") {
                stmts.push_back(parseFileCall());
            } else if (t.type == TokenType::Identifier && pos_ + 3 < tokens_.size() &&
                       tokens_[pos_ + 1].type == TokenType::Dot && tokens_[pos_ + 2].type == TokenType::Identifier &&
                       (tokens_[pos_ + 2].value == "Write" || tokens_[pos_ + 2].value == "Append")) {
                stmts.push_back(parsePathVarFileCall());
            } else if (t.type == TokenType::Identifier && t.value == "random") {
                stmts.push_back(parseRandomCall());
            } else if (t.type == TokenType::Identifier && t.value == "time") {
                stmts.push_back(parseTimeCall());
            } else if (t.type == TokenType::Identifier && t.value == "ui") {
                throw std::runtime_error("std/ui has been removed at line " + std::to_string(t.line));
            } else if (t.type == TokenType::Identifier && pos_ + 1 < tokens_.size() &&
                       tokens_[pos_ + 1].type == TokenType::LParen) {
                stmts.push_back(parseFnCall());
            } else if (t.type == TokenType::Identifier && pos_ + 1 < tokens_.size()) {
                TokenType next = tokens_[pos_ + 1].type;
                if (next == TokenType::LBracket) {
                    stmts.push_back(parseIndexedAssignment());
                } else if (next == TokenType::Assign || next == TokenType::PlusAssign || next == TokenType::MinusAssign ||
                    next == TokenType::StarAssign || next == TokenType::SlashAssign || next == TokenType::PercentAssign) {
                    stmts.push_back(parseAssignment());
                } else if (next == TokenType::PlusPlus || next == TokenType::MinusMinus) {
                    stmts.push_back(parseIncDec());
                } else {
                    throw std::runtime_error("Unexpected token at line " + std::to_string(t.line));
                }
            } else if (t.type == TokenType::Return) {
                stmts.push_back(parseReturn());
            } else if (t.type == TokenType::Break) {
                stmts.push_back(parseBreak());
            } else if (t.type == TokenType::Continue) {
                stmts.push_back(parseContinue());
            } else if (t.type != TokenType::Eof) {
                throw std::runtime_error("Unexpected token at line " + std::to_string(t.line));
            }
        }
        return stmts;
    }

    AstNode parseFnCallExpr() {
        const Token& nameTok = peek();
        advance();
        std::string name = nameTok.value;
        if (!match(TokenType::LParen)) {
            throw std::runtime_error("Expected '(' at line " + std::to_string(peek().line));
        }
        std::vector<AstNode> args;
        if (peek().type != TokenType::RParen) {
            for (;;) {
                args.push_back(parseExpression());
                if (!match(TokenType::Comma)) break;
            }
        }
        if (!match(TokenType::RParen)) {
            throw std::runtime_error("Expected ')' at line " + std::to_string(peek().line));
        }
        AstNode node{AstNode::Type::FnCall, name, {}};
        node.children = std::move(args);
        return node;
    }

    AstNode parseReturn() {
        size_t line = peek().line;
        if (!match(TokenType::Return)) {
            throw std::runtime_error("Expected 'return' at line " + std::to_string(line));
        }
        AstNode expr = parseExpression();
        if (!match(TokenType::Semicolon)) {
            throw std::runtime_error("Expected ';' after return at line " + std::to_string(peek().line));
        }
        AstNode node{AstNode::Type::Return, "", {}};
        node.children.push_back(expr);
        return node;
    }

    AstNode parseBreak() {
        if (!match(TokenType::Break)) {
            throw std::runtime_error("Expected 'break' at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::Semicolon)) {
            throw std::runtime_error("Expected ';' after break at line " + std::to_string(peek().line));
        }
        return {AstNode::Type::Break, "", {}};
    }

    AstNode parseContinue() {
        if (!match(TokenType::Continue)) {
            throw std::runtime_error("Expected 'continue' at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::Semicolon)) {
            throw std::runtime_error("Expected ';' after continue at line " + std::to_string(peek().line));
        }
        return {AstNode::Type::Continue, "", {}};
    }

    AstNode parseIncDec() {
        const Token& nameTok = peek();
        advance();
        std::string name = nameTok.value;
        bool isInc = match(TokenType::PlusPlus);
        if (!isInc && !match(TokenType::MinusMinus)) {
            throw std::runtime_error("Expected '++' or '--' at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::Semicolon)) {
            throw std::runtime_error("Expected ';' after " + std::string(isInc ? "++" : "--") + " at line " + std::to_string(peek().line));
        }
        return {isInc ? AstNode::Type::IncPost : AstNode::Type::DecPost, name, {}};
    }

    AstNode parseFnCall() {
        AstNode node = parseFnCallExpr();
        if (!match(TokenType::Semicolon)) {
            throw std::runtime_error("Expected ';' at line " + std::to_string(peek().line));
        }
        return node;
    }

    AstNode parseIndexedAssignment() {
        const Token& nameTok = peek();
        if (nameTok.type != TokenType::Identifier) {
            throw std::runtime_error("Expected array name at line " + std::to_string(nameTok.line));
        }
        advance();
        std::string name = nameTok.value;
        if (!match(TokenType::LBracket)) {
            throw std::runtime_error("Expected '[' at line " + std::to_string(peek().line));
        }
        AstNode idx = parseExpression();
        if (!match(TokenType::RBracket)) {
            throw std::runtime_error("Expected ']' at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::Assign)) {
            throw std::runtime_error("Expected '=' at line " + std::to_string(peek().line));
        }
        AstNode expr = parseExpression();
        if (!match(TokenType::Semicolon)) {
            throw std::runtime_error("Expected ';' at line " + std::to_string(peek().line));
        }
        AstNode node{AstNode::Type::AssnIndex, name, {idx, expr}};
        return node;
    }

    AstNode parseAssignment() {
        size_t line = peek().line;
        const Token& nameTok = peek();
        if (nameTok.type != TokenType::Identifier) {
            throw std::runtime_error("Expected variable name at line " + std::to_string(line));
        }
        advance();
        std::string name = nameTok.value;
        AstNode::Type assnType = AstNode::Type::Assignment;
        if (match(TokenType::Assign)) {
            assnType = AstNode::Type::Assignment;
        } else if (match(TokenType::PlusAssign)) {
            assnType = AstNode::Type::AssnAdd;
        } else if (match(TokenType::MinusAssign)) {
            assnType = AstNode::Type::AssnSub;
        } else if (match(TokenType::StarAssign)) {
            assnType = AstNode::Type::AssnMul;
        } else if (match(TokenType::SlashAssign)) {
            assnType = AstNode::Type::AssnDiv;
        } else if (match(TokenType::PercentAssign)) {
            assnType = AstNode::Type::AssnMod;
        } else {
            throw std::runtime_error("Expected '=', '+=', '-=', '*=', '/=', or '%=' at line " + std::to_string(peek().line));
        }
        AstNode expr = parseExpression();
        if (!match(TokenType::Semicolon)) {
            throw std::runtime_error("Expected ';' at line " + std::to_string(peek().line));
        }
        AstNode node{assnType, name, {}};
        node.children.push_back(expr);
        return node;
    }

    AstNode parseCondition() {
        AstNode left = parseExpression();
        if (match(TokenType::Equals)) {
            AstNode n{AstNode::Type::CondEq, "", {left, parseExpression()}};
            return n;
        }
        if (match(TokenType::NotEquals)) {
            AstNode n{AstNode::Type::CondNe, "", {left, parseExpression()}};
            return n;
        }
        if (match(TokenType::Less)) {
            AstNode n{AstNode::Type::CondLt, "", {left, parseExpression()}};
            return n;
        }
        if (match(TokenType::LessEq)) {
            AstNode n{AstNode::Type::CondLe, "", {left, parseExpression()}};
            return n;
        }
        if (match(TokenType::Greater)) {
            AstNode n{AstNode::Type::CondGt, "", {left, parseExpression()}};
            return n;
        }
        if (match(TokenType::GreaterEq)) {
            AstNode n{AstNode::Type::CondGe, "", {left, parseExpression()}};
            return n;
        }
        return left;
    }

    AstNode parseLogicalOr() {
        AstNode left = parseLogicalAnd();
        while (match(TokenType::Or)) {
            AstNode n{AstNode::Type::CondOr, "", {left, parseLogicalAnd()}};
            left = n;
        }
        return left;
    }

    AstNode parseLogicalAnd() {
        AstNode left = parseLogicalNot();
        while (match(TokenType::And)) {
            AstNode n{AstNode::Type::CondAnd, "", {left, parseLogicalNot()}};
            left = n;
        }
        return left;
    }

    AstNode parseLogicalNot() {
        if (match(TokenType::Not)) {
            AstNode inner = parseLogicalNot();
            return {AstNode::Type::CondNot, "", {inner}};
        }
        if (match(TokenType::LParen)) {
            AstNode inner = parseLogicalOr();
            if (!match(TokenType::RParen)) {
                throw std::runtime_error("Expected ')' at line " + std::to_string(peek().line));
            }
            return inner;
        }
        return parseCondition();
    }

    AstNode parseIf() {
        size_t line = peek().line;
        if (!match(TokenType::If)) {
            throw std::runtime_error("Expected 'if' at line " + std::to_string(line));
        }
        if (!match(TokenType::LParen)) {
            throw std::runtime_error("Expected '(' at line " + std::to_string(peek().line));
        }
        AstNode cond = parseLogicalOr();
        if (!match(TokenType::RParen)) {
            throw std::runtime_error("Expected ')' at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::LBrace)) {
            throw std::runtime_error("Expected '{' at line " + std::to_string(peek().line));
        }
        AstNode thenBlock{AstNode::Type::Block, "", {}};
        thenBlock.children = parseBlock();
        if (!match(TokenType::RBrace)) {
            throw std::runtime_error("Expected '}' at line " + std::to_string(peek().line));
        }
        AstNode ifNode{AstNode::Type::IfElse, "", {cond, thenBlock}};
        if (match(TokenType::Else)) {
            if (peek().type == TokenType::If) {
                AstNode elseIfPart = parseIf();
                ifNode.children.push_back(elseIfPart);
            } else {
                if (!match(TokenType::LBrace)) {
                    throw std::runtime_error("Expected '{' after else at line " + std::to_string(peek().line));
                }
                AstNode elseBlock{AstNode::Type::Block, "", {}};
                elseBlock.children = parseBlock();
                ifNode.children.push_back(elseBlock);
                if (!match(TokenType::RBrace)) {
                    throw std::runtime_error("Expected '}' at line " + std::to_string(peek().line));
                }
            }
        }
        return ifNode;
    }

    AstNode parseSwitch() {
        size_t line = peek().line;
        if (!match(TokenType::Switch)) {
            throw std::runtime_error("Expected 'switch' at line " + std::to_string(line));
        }
        if (!match(TokenType::LParen)) {
            throw std::runtime_error("Expected '(' after switch at line " + std::to_string(peek().line));
        }
        AstNode expr = parseExpression();
        if (!match(TokenType::RParen)) {
            throw std::runtime_error("Expected ')' at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::LBrace)) {
            throw std::runtime_error("Expected '{' at line " + std::to_string(peek().line));
        }
        AstNode switchNode{AstNode::Type::Switch, "", {expr}};
        bool hasDefault = false;
        bool hasIntCase = false;
        bool hasStringCase = false;
        while (pos_ < tokens_.size() && peek().type != TokenType::RBrace) {
            if (match(TokenType::Case)) {
                const Token& valTok = peek();
                AstNode caseNode{AstNode::Type::SwitchCase, "", {}};
                if (valTok.type == TokenType::Number) {
                    caseNode.value = valTok.value;
                    hasIntCase = true;
                    advance();
                } else if (valTok.type == TokenType::String) {
                    caseNode.initValue = valTok.value;
                    caseNode.caseIsString = true;
                    hasStringCase = true;
                    advance();
                } else {
                    throw std::runtime_error("case value must be an integer or string literal at line " + std::to_string(valTok.line));
                }
                if (!match(TokenType::Colon)) {
                    throw std::runtime_error("Expected ':' after case value at line " + std::to_string(peek().line));
                }
                caseNode.children = parseSwitchBody();
                switchNode.children.push_back(caseNode);
            } else if (match(TokenType::Default)) {
                if (hasDefault) {
                    throw std::runtime_error("Duplicate default at line " + std::to_string(peek().line));
                }
                hasDefault = true;
                if (!match(TokenType::Colon)) {
                    throw std::runtime_error("Expected ':' after default at line " + std::to_string(peek().line));
                }
                AstNode defaultNode{AstNode::Type::SwitchCase, "default", {}};
                defaultNode.children = parseSwitchBody();
                switchNode.children.push_back(defaultNode);
            } else {
                throw std::runtime_error("Expected 'case' or 'default' at line " + std::to_string(peek().line));
            }
        }
        if (!match(TokenType::RBrace)) {
            throw std::runtime_error("Expected '}' at line " + std::to_string(peek().line));
        }
        if (hasIntCase && hasStringCase) {
            throw std::runtime_error("switch cannot mix int and string cases at line " + std::to_string(line));
        }
        return switchNode;
    }

    std::vector<AstNode> parseSwitchBody() {
        std::vector<AstNode> stmts;
        while (pos_ < tokens_.size()) {
            const Token& t = peek();
            if (t.type == TokenType::RBrace || t.type == TokenType::Case || t.type == TokenType::Default) break;
            if (t.type == TokenType::Let) {
                stmts.push_back(parseVariable());
            } else if (t.type == TokenType::If) {
                stmts.push_back(parseIf());
            } else if (t.type == TokenType::Switch) {
                stmts.push_back(parseSwitch());
            } else if (t.type == TokenType::While) {
                stmts.push_back(parseWhile());
            } else if (t.type == TokenType::For) {
                stmts.push_back(parseFor());
            } else if (t.type == TokenType::Identifier && t.value == "io") {
                stmts.push_back(parseIoCall());
            } else if (t.type == TokenType::Identifier && t.value == "os") {
                stmts.push_back(parseOsCall());
            } else if (t.type == TokenType::Identifier && t.value == "dll") {
                stmts.push_back(parseDllCall());
            } else if (t.type == TokenType::Identifier && t.value == "file") {
                stmts.push_back(parseFileCall());
            } else if (t.type == TokenType::Identifier && pos_ + 3 < tokens_.size() &&
                       tokens_[pos_ + 1].type == TokenType::Dot && tokens_[pos_ + 2].type == TokenType::Identifier &&
                       (tokens_[pos_ + 2].value == "Write" || tokens_[pos_ + 2].value == "Append")) {
                stmts.push_back(parsePathVarFileCall());
            } else if (t.type == TokenType::Identifier && t.value == "random") {
                stmts.push_back(parseRandomCall());
            } else if (t.type == TokenType::Identifier && t.value == "time") {
                stmts.push_back(parseTimeCall());
            } else if (t.type == TokenType::Identifier && t.value == "ui") {
                throw std::runtime_error("std/ui has been removed at line " + std::to_string(t.line));
            } else if (t.type == TokenType::Identifier && pos_ + 1 < tokens_.size() &&
                       tokens_[pos_ + 1].type == TokenType::LParen) {
                stmts.push_back(parseFnCall());
            } else if (t.type == TokenType::Identifier && pos_ + 1 < tokens_.size()) {
                TokenType next = tokens_[pos_ + 1].type;
                if (next == TokenType::LBracket) {
                    stmts.push_back(parseIndexedAssignment());
                } else if (next == TokenType::Assign || next == TokenType::PlusAssign || next == TokenType::MinusAssign ||
                    next == TokenType::StarAssign || next == TokenType::SlashAssign || next == TokenType::PercentAssign) {
                    stmts.push_back(parseAssignment());
                } else if (next == TokenType::PlusPlus || next == TokenType::MinusMinus) {
                    stmts.push_back(parseIncDec());
                } else {
                    throw std::runtime_error("Unexpected token at line " + std::to_string(t.line));
                }
            } else if (t.type == TokenType::Return) {
                stmts.push_back(parseReturn());
            } else if (t.type == TokenType::Break) {
                stmts.push_back(parseBreak());
            } else if (t.type == TokenType::Continue) {
                stmts.push_back(parseContinue());
            } else if (t.type != TokenType::Eof) {
                throw std::runtime_error("Unexpected token at line " + std::to_string(t.line));
            } else {
                break;
            }
        }
        return stmts;
    }

    AstNode parseWhile() {
        size_t line = peek().line;
        if (!match(TokenType::While)) {
            throw std::runtime_error("Expected 'while' at line " + std::to_string(line));
        }
        if (!match(TokenType::LParen)) {
            throw std::runtime_error("Expected '(' at line " + std::to_string(peek().line));
        }
        AstNode cond = parseLogicalOr();
        if (!match(TokenType::RParen)) {
            throw std::runtime_error("Expected ')' at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::LBrace)) {
            throw std::runtime_error("Expected '{' at line " + std::to_string(peek().line));
        }
        AstNode block{AstNode::Type::Block, "", {}};
        block.children = parseBlock();
        if (!match(TokenType::RBrace)) {
            throw std::runtime_error("Expected '}' at line " + std::to_string(peek().line));
        }
        return {AstNode::Type::While, "", {cond, block}};
    }

    AstNode parseFor() {
        size_t line = peek().line;
        if (!match(TokenType::For)) {
            throw std::runtime_error("Expected 'for' at line " + std::to_string(line));
        }
        if (!match(TokenType::LParen)) {
            throw std::runtime_error("Expected '(' after for at line " + std::to_string(peek().line));
        }
        const Token& nameTok = peek();
        if (nameTok.type != TokenType::Identifier) {
            throw std::runtime_error("Expected loop variable name at line " + std::to_string(nameTok.line));
        }
        advance();
        std::string varName = nameTok.value;
        if (!match(TokenType::Comma)) {
            throw std::runtime_error("Expected ',' in for loop at line " + std::to_string(peek().line));
        }
        AstNode countExpr = parseExpression();
        if (!match(TokenType::RParen)) {
            throw std::runtime_error("Expected ')' after for loop count at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::LBrace)) {
            throw std::runtime_error("Expected '{' at line " + std::to_string(peek().line));
        }
        AstNode block{AstNode::Type::Block, "", {}};
        block.children = parseBlock();
        if (!match(TokenType::RBrace)) {
            throw std::runtime_error("Expected '}' at line " + std::to_string(peek().line));
        }
        AstNode forNode{AstNode::Type::For, varName, {countExpr, block}};
        return forNode;
    }

    AstNode parseExpression() {
        AstNode left = parseTerm();
        while (true) {
            if (match(TokenType::Plus)) {
                AstNode n{AstNode::Type::ExprAdd, "", {left, parseTerm()}};
                left = n;
            } else if (match(TokenType::Minus)) {
                AstNode n{AstNode::Type::ExprSub, "", {left, parseTerm()}};
                left = n;
            } else {
                break;
            }
        }
        return left;
    }

    AstNode parseTerm() {
        AstNode left = parseFactor();
        while (true) {
            if (match(TokenType::Star)) {
                AstNode n{AstNode::Type::ExprMul, "", {left, parseFactor()}};
                left = n;
            } else if (match(TokenType::Slash)) {
                AstNode n{AstNode::Type::ExprDiv, "", {left, parseFactor()}};
                left = n;
            } else if (match(TokenType::Percent)) {
                AstNode n{AstNode::Type::ExprMod, "", {left, parseFactor()}};
                left = n;
            } else {
                break;
            }
        }
        return left;
    }

    AstNode parseArrayLiteral() {
        if (!match(TokenType::LBracket)) {
            throw std::runtime_error("Expected '[' at line " + std::to_string(peek().line));
        }
        AstNode node{AstNode::Type::ExprArrayLiteral, "", {}};
        if (peek().type != TokenType::RBracket) {
            for (;;) {
                node.children.push_back(parseExpression());
                if (!match(TokenType::Comma)) break;
            }
        }
        if (!match(TokenType::RBracket)) {
            throw std::runtime_error("Expected ']' at line " + std::to_string(peek().line));
        }
        return node;
    }

    AstNode parseFactor() {
        if (peek().type == TokenType::LBracket) {
            return parseArrayLiteral();
        }
        if (match(TokenType::Number)) {
            return {AstNode::Type::ExprIntLiteral, tokens_[pos_ - 1].value, {}};
        }
        if (match(TokenType::Float)) {
            return {AstNode::Type::ExprFloatLiteral, tokens_[pos_ - 1].value, {}};
        }
        if (match(TokenType::Char)) {
            return {AstNode::Type::ExprCharLiteral, tokens_[pos_ - 1].value, {}};
        }
        if (match(TokenType::True)) {
            return {AstNode::Type::ExprBoolLiteral, "true", {}};
        }
        if (match(TokenType::False)) {
            return {AstNode::Type::ExprBoolLiteral, "false", {}};
        }
        if (match(TokenType::String)) {
            return {AstNode::Type::ExprStringLiteral, tokens_[pos_ - 1].value, {}};
        }
        if (peek().type == TokenType::Identifier && peek().value == "io" && pos_ + 2 < tokens_.size() &&
            tokens_[pos_ + 1].type == TokenType::Dot && tokens_[pos_ + 2].type == TokenType::Identifier) {
            std::string method = tokens_[pos_ + 2].value;
            if (method == "readln") return parseIoReadlnExpr();
            if (method == "to_int") return parseIoToIntExpr();
        }
        if (peek().type == TokenType::Identifier && peek().value == "os" && pos_ + 2 < tokens_.size() &&
            tokens_[pos_ + 1].type == TokenType::Dot && tokens_[pos_ + 2].type == TokenType::Identifier) {
            std::string method = tokens_[pos_ + 2].value;
            if (method == "getenv") return parseOsGetenv();
            if (method == "platform") return parseOsPlatform();
            if (method == "exe_dir") return parseOsExeDir();
        }
        if (peek().type == TokenType::Identifier && peek().value == "file" && pos_ + 2 < tokens_.size() &&
            tokens_[pos_ + 1].type == TokenType::Dot && tokens_[pos_ + 2].type == TokenType::Identifier) {
            std::string method = tokens_[pos_ + 2].value;
            if (method == "read" || method == "exists") {
                return parseFileReadOrExists(method == "read");
            }
        }
        if (peek().type == TokenType::Identifier && peek().value == "random" && pos_ + 2 < tokens_.size() &&
            tokens_[pos_ + 1].type == TokenType::Dot && tokens_[pos_ + 2].type == TokenType::Identifier &&
            tokens_[pos_ + 2].value == "int") {
            return parseRandomInt();
        }
        if (peek().type == TokenType::Identifier && peek().value == "time" && pos_ + 2 < tokens_.size() &&
            tokens_[pos_ + 1].type == TokenType::Dot && tokens_[pos_ + 2].type == TokenType::Identifier) {
            std::string method = tokens_[pos_ + 2].value;
            if (method == "seconds") return parseTimeSeconds();
            if (method == "milliseconds") return parseTimeMilliseconds();
        }
        if (peek().type == TokenType::Identifier && peek().value == "ui") {
            throw std::runtime_error("std/ui has been removed at line " + std::to_string(peek().line));
        }
        if (peek().type == TokenType::Identifier && pos_ + 1 < tokens_.size() &&
            tokens_[pos_ + 1].type == TokenType::LParen) {
            return parseFnCallExpr();
        }
        if (match(TokenType::Identifier)) {
            std::string name = tokens_[pos_ - 1].value;
            if (match(TokenType::LBracket)) {
                AstNode idx = parseExpression();
                if (!match(TokenType::RBracket)) {
                    throw std::runtime_error("Expected ']' at line " + std::to_string(peek().line));
                }
                return {AstNode::Type::ExprArrayIndex, name, {idx}};
            }
            return {AstNode::Type::ExprVarRef, name, {}};
        }
        if (match(TokenType::LParen)) {
            AstNode e = parseExpression();
            if (!match(TokenType::RParen)) {
                throw std::runtime_error("Expected ')' at line " + std::to_string(peek().line));
            }
            return e;
        }
        throw std::runtime_error("Expected number, variable, or (expression) at line " + std::to_string(peek().line));
    }

    AstNode parseIoCall() {
        size_t line = peek().line;
        if (!match(TokenType::Identifier) || tokens_[pos_ - 1].value != "io") {
            throw std::runtime_error("Expected 'io' at line " + std::to_string(line));
        }
        if (!match(TokenType::Dot)) {
            throw std::runtime_error("Expected '.' at line " + std::to_string(peek().line));
        }
        const Token& methodTok = peek();
        if (methodTok.type != TokenType::Identifier) {
            throw std::runtime_error("Expected 'print' or 'println' at line " + std::to_string(methodTok.line));
        }
        bool isPrintln = (methodTok.value == "println");
        if (methodTok.value != "print" && !isPrintln) {
            throw std::runtime_error("Expected 'print' or 'println' at line " + std::to_string(methodTok.line));
        }
        advance();
        if (!match(TokenType::LParen)) {
            throw std::runtime_error("Expected '(' at line " + std::to_string(peek().line));
        }
        const Token& argTok = peek();
        AstNode result{isPrintln ? AstNode::Type::IoPrintln : AstNode::Type::IoPrint, "", {}};
        if (argTok.type == TokenType::String || argTok.type == TokenType::Number ||
            argTok.type == TokenType::Identifier || argTok.type == TokenType::LParen) {
            result.children.push_back(parseExpression());
        } else {
            throw std::runtime_error("Expected string or expression at line " + std::to_string(argTok.line));
        }
        if (!match(TokenType::RParen)) {
            throw std::runtime_error("Expected ')' at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::Semicolon)) {
            throw std::runtime_error("Expected ';' at line " + std::to_string(peek().line));
        }
        return result;
    }

    AstNode parseOsCall() {
        size_t line = peek().line;
        if (!modules_.hasOs()) {
            throw std::runtime_error("os.system requires #include <std/os> at line " + std::to_string(line));
        }
        if (!match(TokenType::Identifier) || tokens_[pos_ - 1].value != "os") {
            throw std::runtime_error("Expected 'os' at line " + std::to_string(line));
        }
        if (!match(TokenType::Dot)) {
            throw std::runtime_error("Expected '.' at line " + std::to_string(peek().line));
        }
        const Token& methodTok = peek();
        if (methodTok.type != TokenType::Identifier || methodTok.value != "system") {
            throw std::runtime_error("Expected 'system' at line " + std::to_string(methodTok.line));
        }
        advance();
        if (!match(TokenType::LParen)) {
            throw std::runtime_error("Expected '(' at line " + std::to_string(peek().line));
        }
        const Token& argTok = peek();
        AstNode result{AstNode::Type::OsSystem, "", {}};
        if (argTok.type == TokenType::String || argTok.type == TokenType::Identifier ||
            argTok.type == TokenType::LParen) {
            result.children.push_back(parseExpression());
            result.isVarRef = false;
        } else {
            throw std::runtime_error("Expected string or expression at line " + std::to_string(argTok.line));
        }
        if (!match(TokenType::RParen)) {
            throw std::runtime_error("Expected ')' at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::Semicolon)) {
            throw std::runtime_error("Expected ';' at line " + std::to_string(peek().line));
        }
        return result;
    }

    AstNode parseOsGetenv() {
        size_t line = peek().line;
        if (!modules_.hasOs()) {
            throw std::runtime_error("os.getenv requires #include <std/os> at line " + std::to_string(line));
        }
        if (!match(TokenType::Identifier) || tokens_[pos_ - 1].value != "os") {
            throw std::runtime_error("Expected 'os' at line " + std::to_string(line));
        }
        if (!match(TokenType::Dot)) {
            throw std::runtime_error("Expected '.' at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::Identifier) || tokens_[pos_ - 1].value != "getenv") {
            throw std::runtime_error("Expected 'getenv' at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::LParen)) {
            throw std::runtime_error("Expected '(' at line " + std::to_string(peek().line));
        }
        const Token& argTok = peek();
        if (argTok.type != TokenType::String) {
            throw std::runtime_error("Expected string for env var name at line " + std::to_string(argTok.line));
        }
        advance();
        std::string envName = tokens_[pos_ - 1].value;
        if (!match(TokenType::RParen)) {
            throw std::runtime_error("Expected ')' at line " + std::to_string(peek().line));
        }
        return {AstNode::Type::OsGetenv, envName, {}};
    }

    AstNode parseOsPlatform() {
        if (!modules_.hasOs()) {
            throw std::runtime_error("os.platform requires #include <std/os> at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::Identifier) || tokens_[pos_ - 1].value != "os") {
            throw std::runtime_error("Expected 'os' at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::Dot)) {
            throw std::runtime_error("Expected '.' at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::Identifier) || tokens_[pos_ - 1].value != "platform") {
            throw std::runtime_error("Expected 'platform' at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::LParen) || !match(TokenType::RParen)) {
            throw std::runtime_error("Expected '()' after platform at line " + std::to_string(peek().line));
        }
        return {AstNode::Type::OsPlatform, "", {}};
    }

    AstNode parseOsExeDir() {
        if (!modules_.hasOs()) {
            throw std::runtime_error("os.exe_dir requires #include <std/os> at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::Identifier) || tokens_[pos_ - 1].value != "os") {
            throw std::runtime_error("Expected 'os' at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::Dot)) {
            throw std::runtime_error("Expected '.' at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::Identifier) || tokens_[pos_ - 1].value != "exe_dir") {
            throw std::runtime_error("Expected 'exe_dir' at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::LParen) || !match(TokenType::RParen)) {
            throw std::runtime_error("Expected '()' after exe_dir at line " + std::to_string(peek().line));
        }
        return {AstNode::Type::OsExeDir, "", {}};
    }

    AstNode parseIoReadlnExpr() {
        if (!match(TokenType::Identifier) || tokens_[pos_ - 1].value != "io") {
            throw std::runtime_error("Expected 'io' at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::Dot)) {
            throw std::runtime_error("Expected '.' at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::Identifier) || tokens_[pos_ - 1].value != "readln") {
            throw std::runtime_error("Expected io.readln at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::LParen) || !match(TokenType::RParen)) {
            throw std::runtime_error("Expected '()' after readln at line " + std::to_string(peek().line));
        }
        return {AstNode::Type::IoReadln, "", {}};
    }

    AstNode parseIoToIntExpr() {
        if (!match(TokenType::Identifier) || tokens_[pos_ - 1].value != "io") {
            throw std::runtime_error("Expected 'io' at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::Dot)) {
            throw std::runtime_error("Expected '.' at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::Identifier) || tokens_[pos_ - 1].value != "to_int") {
            throw std::runtime_error("Expected io.to_int at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::LParen)) {
            throw std::runtime_error("Expected '(' at line " + std::to_string(peek().line));
        }
        AstNode arg = parseExpression();
        if (!match(TokenType::RParen)) {
            throw std::runtime_error("Expected ')' at line " + std::to_string(peek().line));
        }
        return {AstNode::Type::IoToInt, "", {arg}};
    }

    AstNode parseFileReadOrExists(bool isRead) {
        if (!modules_.hasFile()) {
            throw std::runtime_error("file.read/file.exists requires #include <std/file> at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::Identifier) || tokens_[pos_ - 1].value != "file") {
            throw std::runtime_error("Expected 'file' at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::Dot)) {
            throw std::runtime_error("Expected '.' at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::Identifier) || tokens_[pos_ - 1].value != (isRead ? "read" : "exists")) {
            throw std::runtime_error(std::string("Expected file.") + (isRead ? "read" : "exists") + " at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::LParen)) {
            throw std::runtime_error("Expected '(' at line " + std::to_string(peek().line));
        }
        AstNode pathArg = parseExpression();
        if (!match(TokenType::RParen)) {
            throw std::runtime_error("Expected ')' at line " + std::to_string(peek().line));
        }
        return {isRead ? AstNode::Type::FileRead : AstNode::Type::FileExists, "", {pathArg}};
    }

    AstNode parseRandomInt() {
        if (!modules_.hasRandom()) {
            throw std::runtime_error("random.int requires #include <std/random> at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::Identifier) || tokens_[pos_ - 1].value != "random") {
            throw std::runtime_error("Expected 'random' at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::Dot)) {
            throw std::runtime_error("Expected '.' at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::Identifier) || tokens_[pos_ - 1].value != "int") {
            throw std::runtime_error("Expected random.int at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::LParen)) {
            throw std::runtime_error("Expected '(' at line " + std::to_string(peek().line));
        }
        AstNode minArg = parseExpression();
        if (!match(TokenType::Comma)) {
            throw std::runtime_error("Expected ',' in random.int(min, max) at line " + std::to_string(peek().line));
        }
        AstNode maxArg = parseExpression();
        if (!match(TokenType::RParen)) {
            throw std::runtime_error("Expected ')' at line " + std::to_string(peek().line));
        }
        return {AstNode::Type::RandomInt, "", {minArg, maxArg}};
    }

    AstNode parseDllCall() {
        size_t line = peek().line;
        if (!modules_.hasDll()) {
            throw std::runtime_error("dll.call requires #include <std/dll> at line " + std::to_string(line));
        }
        if (!match(TokenType::Identifier) || tokens_[pos_ - 1].value != "dll") {
            throw std::runtime_error("Expected 'dll' at line " + std::to_string(line));
        }
        if (!match(TokenType::Dot)) {
            throw std::runtime_error("Expected '.' at line " + std::to_string(peek().line));
        }
        const Token& methodTok = peek();
        if (methodTok.type != TokenType::Identifier || methodTok.value != "call") {
            throw std::runtime_error("Expected 'call' at line " + std::to_string(methodTok.line));
        }
        advance();
        if (!match(TokenType::LParen)) {
            throw std::runtime_error("Expected '(' at line " + std::to_string(peek().line));
        }
        const Token& handleTok = peek();
        if (handleTok.type != TokenType::Identifier) {
            throw std::runtime_error("Expected handle variable at line " + std::to_string(handleTok.line));
        }
        advance();
        std::string handleVar = handleTok.value;
        if (!match(TokenType::Comma)) {
            throw std::runtime_error("Expected ',' at line " + std::to_string(peek().line));
        }
        const Token& symTok = peek();
        if (symTok.type != TokenType::String) {
            throw std::runtime_error("Expected symbol name string at line " + std::to_string(symTok.line));
        }
        advance();
        if (!match(TokenType::RParen)) {
            throw std::runtime_error("Expected ')' at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::Semicolon)) {
            throw std::runtime_error("Expected ';' at line " + std::to_string(peek().line));
        }
        AstNode result{AstNode::Type::DllCall, symTok.value, {}};
        result.children.push_back({AstNode::Type::ExprVarRef, handleVar, {}});
        return result;
    }

    AstNode parseFileCall() {
        size_t line = peek().line;
        if (!modules_.hasFile()) {
            throw std::runtime_error("file.* requires #include <std/file> at line " + std::to_string(line));
        }
        if (!match(TokenType::Identifier) || tokens_[pos_ - 1].value != "file") {
            throw std::runtime_error("Expected 'file' at line " + std::to_string(line));
        }
        if (!match(TokenType::Dot)) {
            throw std::runtime_error("Expected '.' at line " + std::to_string(peek().line));
        }
        const Token& methodTok = peek();
        if (methodTok.type != TokenType::Identifier) {
            throw std::runtime_error("Expected file method (read, write, append, exists) at line " + std::to_string(methodTok.line));
        }
        std::string method = methodTok.value;
        if (method != "read" && method != "write" && method != "append" && method != "exists") {
            throw std::runtime_error("Expected file.read, file.write, file.append, or file.exists at line " + std::to_string(methodTok.line));
        }
        advance();
        if (!match(TokenType::LParen)) {
            throw std::runtime_error("Expected '(' at line " + std::to_string(peek().line));
        }
        AstNode pathArg = parseExpression();
        AstNode node{method == "read" ? AstNode::Type::FileRead : (method == "write" ? AstNode::Type::FileWrite :
            (method == "append" ? AstNode::Type::FileAppend : AstNode::Type::FileExists)), "", {}};
        node.children.push_back(pathArg);
        if (method == "write" || method == "append") {
            if (!match(TokenType::Comma)) {
                throw std::runtime_error("Expected ',' before content at line " + std::to_string(peek().line));
            }
            node.children.push_back(parseExpression());
        }
        if (!match(TokenType::RParen)) {
            throw std::runtime_error("Expected ')' at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::Semicolon)) {
            throw std::runtime_error("Expected ';' at line " + std::to_string(peek().line));
        }
        return node;
    }

    AstNode parsePathVarFileCall() {
        size_t line = peek().line;
        if (!modules_.hasFile()) {
            throw std::runtime_error("pathVar.Write/Append requires #include <std/file> at line " + std::to_string(line));
        }
        const Token& nameTok = peek();
        if (nameTok.type != TokenType::Identifier) {
            throw std::runtime_error("Expected variable name at line " + std::to_string(line));
        }
        advance();
        std::string pathVar = nameTok.value;
        if (!match(TokenType::Dot)) {
            throw std::runtime_error("Expected '.' at line " + std::to_string(peek().line));
        }
        const Token& methodTok = peek();
        if (methodTok.type != TokenType::Identifier || (methodTok.value != "Write" && methodTok.value != "Append")) {
            throw std::runtime_error("Expected .Write or .Append at line " + std::to_string(peek().line));
        }
        bool isAppend = (methodTok.value == "Append");
        advance();
        if (!match(TokenType::LParen)) {
            throw std::runtime_error("Expected '(' at line " + std::to_string(peek().line));
        }
        AstNode pathArg{AstNode::Type::ExprVarRef, pathVar, {}};
        AstNode contentArg = parseExpression();
        AstNode node{isAppend ? AstNode::Type::FileAppend : AstNode::Type::FileWrite, "", {}};
        node.children.push_back(pathArg);
        node.children.push_back(contentArg);
        if (!match(TokenType::RParen)) {
            throw std::runtime_error("Expected ')' at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::Semicolon)) {
            throw std::runtime_error("Expected ';' at line " + std::to_string(peek().line));
        }
        return node;
    }

    AstNode parseRandomCall() {
        size_t line = peek().line;
        if (!modules_.hasRandom()) {
            throw std::runtime_error("random.* requires #include <std/random> at line " + std::to_string(line));
        }
        if (!match(TokenType::Identifier) || tokens_[pos_ - 1].value != "random") {
            throw std::runtime_error("Expected 'random' at line " + std::to_string(line));
        }
        if (!match(TokenType::Dot)) {
            throw std::runtime_error("Expected '.' at line " + std::to_string(peek().line));
        }
        const Token& methodTok = peek();
        if (methodTok.type != TokenType::Identifier) {
            throw std::runtime_error("Expected random method (int, seed) at line " + std::to_string(methodTok.line));
        }
        std::string method = methodTok.value;
        if (method != "int" && method != "seed") {
            throw std::runtime_error("Expected random.int or random.seed at line " + std::to_string(methodTok.line));
        }
        advance();
        if (!match(TokenType::LParen)) {
            throw std::runtime_error("Expected '(' at line " + std::to_string(peek().line));
        }
        AstNode arg1 = parseExpression();
        AstNode node{method == "int" ? AstNode::Type::RandomInt : AstNode::Type::RandomSeed, "", {}};
        node.children.push_back(arg1);
        if (method == "int") {
            if (!match(TokenType::Comma)) {
                throw std::runtime_error("Expected ',' in random.int(min, max) at line " + std::to_string(peek().line));
            }
            node.children.push_back(parseExpression());
        }
        if (!match(TokenType::RParen)) {
            throw std::runtime_error("Expected ')' at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::Semicolon)) {
            throw std::runtime_error("Expected ';' at line " + std::to_string(peek().line));
        }
        return node;
    }

    AstNode parseTimeCall() {
        size_t line = peek().line;
        if (!modules_.hasTime()) {
            throw std::runtime_error("time.* requires #include <std/time> at line " + std::to_string(line));
        }
        if (!match(TokenType::Identifier) || tokens_[pos_ - 1].value != "time") {
            throw std::runtime_error("Expected 'time' at line " + std::to_string(line));
        }
        if (!match(TokenType::Dot)) {
            throw std::runtime_error("Expected '.' at line " + std::to_string(peek().line));
        }
        const Token& methodTok = peek();
        if (methodTok.type != TokenType::Identifier || methodTok.value != "sleep") {
            throw std::runtime_error("Expected time.sleep at line " + std::to_string(peek().line));
        }
        advance();
        if (!match(TokenType::LParen)) {
            throw std::runtime_error("Expected '(' at line " + std::to_string(peek().line));
        }
        AstNode durationArg = parseExpression();
        if (!match(TokenType::RParen)) {
            throw std::runtime_error("Expected ')' at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::Semicolon)) {
            throw std::runtime_error("Expected ';' at line " + std::to_string(peek().line));
        }
        AstNode node{AstNode::Type::TimeSleep, "", {}};
        node.children.push_back(durationArg);
        return node;
    }

    AstNode parseTimeSeconds() {
        if (!modules_.hasTime()) {
            throw std::runtime_error("time.seconds requires #include <std/time> at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::Identifier) || tokens_[pos_ - 1].value != "time") {
            throw std::runtime_error("Expected 'time' at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::Dot)) {
            throw std::runtime_error("Expected '.' at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::Identifier) || tokens_[pos_ - 1].value != "seconds") {
            throw std::runtime_error("Expected time.seconds at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::LParen)) {
            throw std::runtime_error("Expected '(' at line " + std::to_string(peek().line));
        }
        AstNode arg = parseExpression();
        if (!match(TokenType::RParen)) {
            throw std::runtime_error("Expected ')' at line " + std::to_string(peek().line));
        }
        return {AstNode::Type::TimeSeconds, "", {arg}};
    }

    AstNode parseTimeMilliseconds() {
        if (!modules_.hasTime()) {
            throw std::runtime_error("time.milliseconds requires #include <std/time> at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::Identifier) || tokens_[pos_ - 1].value != "time") {
            throw std::runtime_error("Expected 'time' at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::Dot)) {
            throw std::runtime_error("Expected '.' at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::Identifier) || tokens_[pos_ - 1].value != "milliseconds") {
            throw std::runtime_error("Expected time.milliseconds at line " + std::to_string(peek().line));
        }
        if (!match(TokenType::LParen)) {
            throw std::runtime_error("Expected '(' at line " + std::to_string(peek().line));
        }
        AstNode arg = parseExpression();
        if (!match(TokenType::RParen)) {
            throw std::runtime_error("Expected ')' at line " + std::to_string(peek().line));
        }
        return {AstNode::Type::TimeMilliseconds, "", {arg}};
    }

    AstNode parseVariable() {
        size_t line = peek().line;
        if (!match(TokenType::Let)) {
            throw std::runtime_error("Expected 'let' at line " + std::to_string(line));
        }
        bool isConstVar = match(TokenType::Const);
        const Token& nameTok = peek();
        if (nameTok.type != TokenType::Identifier) {
            throw std::runtime_error("Expected variable name at line " + std::to_string(nameTok.line));
        }
        advance();
        std::string name = nameTok.value;
        std::string declType = "";
        bool isFixedArray = false;
        std::string arraySize = "";
        if (match(TokenType::Colon)) {
            const Token& t = peek();
            if (t.type != TokenType::Identifier) {
                throw std::runtime_error("Expected variable type at line " + std::to_string(t.line));
            }
            if (t.value == "unsigned") {
                advance();
                const Token& t2 = peek();
                if (t2.type != TokenType::Identifier || t2.value != "char") {
                    throw std::runtime_error("Expected 'char' after 'unsigned' at line " + std::to_string(t2.line));
                }
                advance();
                declType = "unsigned char";
            } else if (t.value == "int") { advance(); declType = "int"; }
            else if (t.value == "string") { advance(); declType = "string"; }
            else if (t.value == "bool") { advance(); declType = "bool"; }
            else if (t.value == "float") { advance(); declType = "float"; }
            else if (t.value == "char") { advance(); declType = "char"; }
            else throw std::runtime_error("Variable type must be int, string, bool, float, char, or unsigned char at line " + std::to_string(t.line));
            if (match(TokenType::LBracket)) {
                const Token& sizeTok = peek();
                if (sizeTok.type != TokenType::Number) {
                    throw std::runtime_error("Fixed array size must be a constant integer at line " + std::to_string(sizeTok.line));
                }
                arraySize = sizeTok.value;
                advance();
                if (!match(TokenType::RBracket)) {
                    throw std::runtime_error("Expected ']' after array size at line " + std::to_string(peek().line));
                }
                isFixedArray = true;
                if (declType == "string" || declType == "bool" || declType == "float") {
                    throw std::runtime_error("Fixed array only supports int, char, or unsigned char at line " + std::to_string(t.line));
                }
            }
        }
        if (!match(TokenType::Assign)) {
            if (isConstVar && !isFixedArray) {
                throw std::runtime_error("const variable must have an initializer at line " + std::to_string(peek().line));
            }
            if (!match(TokenType::Semicolon)) {
                throw std::runtime_error("Expected '=' or ';' at line " + std::to_string(peek().line));
            }
            AstNode node{AstNode::Type::Variable, name, {}};
            node.initUninitialized = true;
            node.declType = declType;
            node.initIsInt = (declType == "int");
            node.initIsBool = (declType == "bool");
            node.initIsFloat = (declType == "float");
            node.initIsChar = (declType == "char");
            node.isFixedArray = isFixedArray;
            node.arraySize = arraySize;
            return node;
        }
        if (isFixedArray) {
            throw std::runtime_error("Fixed array cannot have an initializer; use let name: type[size]; at line " + std::to_string(peek().line));
        }
        const Token& initTok = peek();
        AstNode node{AstNode::Type::Variable, name, {}};
        if (initTok.type == TokenType::String) {
            advance();
            node.initValue = initTok.value;
            node.initIsInt = false;
        } else if (initTok.type == TokenType::Identifier && initTok.value == "io") {
            node.children.push_back(parseExpression());
            if (node.children.back().type == AstNode::Type::IoReadln) {
                node.initFromReadln = true;
                node.children.clear();
            }
            node.initIsInt = !exprProducesString(node.children.empty() ? AstNode{AstNode::Type::IoReadln, "", {}} : node.children.back());
        } else if (initTok.type == TokenType::Identifier && initTok.value == "file") {
            if (!modules_.hasFile()) {
                throw std::runtime_error("file.read requires #include <std/file> at line " + std::to_string(line));
            }
            advance();
            if (!match(TokenType::Dot)) {
                throw std::runtime_error("Expected '.' at line " + std::to_string(peek().line));
            }
            const Token& methodTok = peek();
            if (methodTok.type != TokenType::Identifier || methodTok.value != "read") {
                throw std::runtime_error("Expected file.read at line " + std::to_string(peek().line));
            }
            advance();
            if (!match(TokenType::LParen)) {
                throw std::runtime_error("Expected '(' at line " + std::to_string(peek().line));
            }
            node.children.push_back(parseExpression());
            if (!match(TokenType::RParen)) {
                throw std::runtime_error("Expected ')' at line " + std::to_string(peek().line));
            }
            node.initFromFileRead = true;
            node.initIsInt = false;
        } else if (initTok.type == TokenType::Identifier && initTok.value == "dll") {
            if (!modules_.hasDll()) {
                throw std::runtime_error("dll.load requires #include <std/dll> at line " + std::to_string(line));
            }
            advance();
            if (!match(TokenType::Dot)) {
                throw std::runtime_error("Expected '.' at line " + std::to_string(peek().line));
            }
            const Token& methodTok = peek();
            if (methodTok.type != TokenType::Identifier || methodTok.value != "load") {
                throw std::runtime_error("Expected 'load' at line " + std::to_string(methodTok.line));
            }
            advance();
            if (!match(TokenType::LParen)) {
                throw std::runtime_error("Expected '(' at line " + std::to_string(peek().line));
            }
            const Token& pathTok = peek();
            if (pathTok.type != TokenType::String) {
                throw std::runtime_error("Expected string path at line " + std::to_string(pathTok.line));
            }
            advance();
            node.initValue = pathTok.value;
            node.initFromDllLoad = true;
            if (!match(TokenType::RParen)) {
                throw std::runtime_error("Expected ')' at line " + std::to_string(peek().line));
            }
        } else if (initTok.type == TokenType::LBracket) {
            node.children.push_back(parseArrayLiteral());
            node.initFromArray = true;
            node.initIsInt = false;
        } else if (initTok.type == TokenType::Number || initTok.type == TokenType::Float || initTok.type == TokenType::Char ||
                   initTok.type == TokenType::Identifier || initTok.type == TokenType::LParen ||
                   initTok.type == TokenType::True || initTok.type == TokenType::False) {
            node.children.push_back(parseExpression());
            if (node.children.back().type == AstNode::Type::ExprBoolLiteral) {
                node.initIsBool = true;
            } else if (node.children.back().type == AstNode::Type::ExprFloatLiteral) {
                node.initIsFloat = true;
            } else if (node.children.back().type == AstNode::Type::ExprCharLiteral) {
                node.initIsChar = true;
            } else {
                node.initIsInt = !exprProducesString(node.children.back());
            }
        } else {
            throw std::runtime_error("Expected number, string, expression, io.readln(), or array at line " + std::to_string(initTok.line));
        }
        if (!declType.empty()) {
            node.declType = declType;
            node.initIsInt = (declType == "int");
            node.initIsBool = (declType == "bool");
            node.initIsFloat = (declType == "float");
            node.initIsChar = (declType == "char");
        }
        if (!match(TokenType::Semicolon)) {
            throw std::runtime_error("Expected ';' at line " + std::to_string(peek().line));
        }
        node.isConst = isConstVar;
        return node;
    }
};

}  // namespace nexa
