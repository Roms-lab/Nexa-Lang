#pragma once

#include "Parser.hpp"
#include "Modules.hpp"
#include <string>
#include <sstream>
#include <cstdio>
#include <map>
#include <functional>

namespace nexa {

// Converts Nexa AST to C++ source code
class Transpiler {
public:
    Transpiler(const std::vector<AstNode>& ast, const Modules& modules, bool preserveNames = false, bool buildDll = false)
        : ast_(ast), modules_(modules), preserveNames_(preserveNames), buildDll_(buildDll) {}

    std::string transpile() {
        std::ostringstream out;

        // C++ includes from enabled modules
        if (buildDll_) {
            out << "#ifdef _WIN32\n";
            out << "#define NEXA_EXPORT __declspec(dllexport)\n";
            out << "#else\n";
            out << "#define NEXA_EXPORT __attribute__((visibility(\"default\")))\n";
            out << "#endif\n\n";
        }
        out << modules_.getCppIncludes();
        bool needsString = false;
        std::function<void(const AstNode&)> checkNeedsString = [&](const AstNode& n) {
            if (n.type == AstNode::Type::Variable && (n.initUninitialized || n.initFromReadln || n.initFromFileRead || (!n.initIsInt && !n.initFromDllLoad && n.children.empty()))) needsString = true;
            if (n.type == AstNode::Type::Variable && !n.children.empty() && exprProducesString(n.children[0])) needsString = true;
            if ((n.type == AstNode::Type::IoPrintln || n.type == AstNode::Type::IoPrint) && !n.children.empty() && exprProducesString(n.children[0])) needsString = true;
            if (n.type == AstNode::Type::OsSystem && !n.children.empty() && exprProducesString(n.children[0])) needsString = true;
            if (n.type == AstNode::Type::While && n.children.size() > 1) { for (const auto& c : n.children[1].children) checkNeedsString(c); }
            if (n.type == AstNode::Type::For && n.children.size() > 1) { for (const auto& c : n.children[1].children) checkNeedsString(c); }
            if (n.type == AstNode::Type::IfElse) { for (size_t i = 1; i < n.children.size(); i++) { for (const auto& c : n.children[i].children) checkNeedsString(c); } }
            if (n.type == AstNode::Type::Block) { for (const auto& c : n.children) checkNeedsString(c); }
        };
        for (const AstNode& node : ast_) {
            if (node.type == AstNode::Type::MainFunction || node.type == AstNode::Type::Function) {
                for (const AstNode& c : node.children) checkNeedsString(c);
                for (const std::string& pt : node.paramTypes) {
                    if (pt == "string") needsString = true;
                }
            }
            if (node.type == AstNode::Type::Variable && (node.declType == "string" || (node.initUninitialized && node.declType != "int") || node.initFromFileRead || (!node.initIsInt && !node.initFromDllLoad && node.children.empty()))) needsString = true;
            if (node.type == AstNode::Type::Variable && !node.children.empty() && exprProducesString(node.children[0])) needsString = true;
        }
        bool needsVector = false;
        std::function<void(const AstNode&)> checkNeedsVector = [&](const AstNode& n) {
            if (n.type == AstNode::Type::Variable && n.initFromArray) needsVector = true;
            if (n.type == AstNode::Type::Variable && !n.children.empty() && n.children[0].type == AstNode::Type::ExprArrayLiteral) needsVector = true;
            if (n.type == AstNode::Type::ExprArrayLiteral || n.type == AstNode::Type::ExprArrayIndex || n.type == AstNode::Type::AssnIndex) needsVector = true;
            if (n.type == AstNode::Type::While && n.children.size() > 1) { for (const auto& c : n.children[1].children) checkNeedsVector(c); }
            if (n.type == AstNode::Type::For && n.children.size() > 1) { for (const auto& c : n.children[1].children) checkNeedsVector(c); }
            if (n.type == AstNode::Type::IfElse) { for (size_t i = 1; i < n.children.size(); i++) { for (const auto& c : n.children[i].children) checkNeedsVector(c); } }
            if (n.type == AstNode::Type::Block) { for (const auto& c : n.children) checkNeedsVector(c); }
        };
        for (const AstNode& node : ast_) {
            if (node.type == AstNode::Type::MainFunction || node.type == AstNode::Type::Function) {
                for (const auto& c : node.children) checkNeedsVector(c);
            }
            if (node.type == AstNode::Type::Variable && node.initFromArray) needsVector = true;
            if (node.type == AstNode::Type::Variable && !node.children.empty() && node.children[0].type == AstNode::Type::ExprArrayLiteral) needsVector = true;
        }
        if (needsString) out << "#include <string>\n";
        if (needsVector) out << "#include <vector>\n";
        if (!modules_.getCppIncludes().empty() || needsString || needsVector) out << "\n";

        // Build function name map (for expressions in globals and functions)
        std::map<std::string, int> fnIndex;
        int idx = 0;
        for (const AstNode& node : ast_) {
            if (node.type == AstNode::Type::Function) {
                fnIndex[node.value] = idx++;
            }
        }

        auto fnName = [&](const std::string& name) -> std::string {
            if (preserveNames_) return name;
            auto it = fnIndex.find(name);
            if (it != fnIndex.end()) return "__nexa_fn_" + std::to_string(it->second);
            return name;  // fallback (e.g. main)
        };

        // Emit global variables
        std::map<std::string, std::string> globalVarMap;
        std::map<std::string, bool> globalVarIsString;
        std::map<std::string, bool> globalVarIsFloat;
        std::map<std::string, bool> globalVarIsChar;
        std::map<std::string, bool> globalVarIsArray;
        std::map<std::string, bool> globalVarIsConst;
        int globalIdx = 0;
        for (const AstNode& node : ast_) {
            if (node.type != AstNode::Type::Variable) continue;
            if (node.initFromReadln) {
                throw std::runtime_error("Global variable cannot use io.readln()");
            }
            if (node.initFromFileRead) {
                throw std::runtime_error("Global variable cannot use file.read()");
            }
            if (node.initFromDllLoad) {
                throw std::runtime_error("Global variable cannot use dll.load()");
            }
            std::string vname = preserveNames_ ? node.value : ("__nexa_g_" + std::to_string(globalIdx++));
            if (!node.children.empty() && node.children[0].type == AstNode::Type::OsGetenv) {
                const std::string& envName = node.children[0].value;
                out << "const char* __nexa_ge_" << globalIdx << " = getenv(\"" << escapeString(envName) << "\");\n";
                out << "std::string " << vname << " = __nexa_ge_" << globalIdx << " ? __nexa_ge_" << globalIdx << " : \"\";\n";
                globalVarMap[node.value] = vname;
                globalVarIsString[node.value] = true;
                globalVarIsArray[node.value] = false;
                globalIdx++;
                continue;
            }
            if (!node.children.empty() && node.children[0].type == AstNode::Type::OsPlatform) {
                out << "std::string " << vname << " = __nexa_os_platform();\n";
                globalVarMap[node.value] = vname;
                globalVarIsString[node.value] = true;
                globalVarIsArray[node.value] = false;
                continue;
            }
            if (!node.children.empty() && node.children[0].type == AstNode::Type::OsExeDir) {
                out << "std::string " << vname << " = __nexa_exe_dir();\n";
                globalVarMap[node.value] = vname;
                globalVarIsString[node.value] = true;
                globalVarIsArray[node.value] = false;
                continue;
            }
            globalVarMap[node.value] = vname;
            globalVarIsConst[node.value] = node.isConst;
            globalVarIsFloat[node.value] = (!node.declType.empty() && node.declType == "float") || node.initIsFloat;
            globalVarIsChar[node.value] = (!node.declType.empty() && node.declType == "char") || node.initIsChar;
            bool isArray = node.initFromArray || (!node.children.empty() && node.children[0].type == AstNode::Type::ExprArrayLiteral);
            bool isStr = !node.declType.empty() ? (node.declType == "string") : (node.initUninitialized || (!node.initIsInt && !node.initIsBool && !node.initIsFloat && !node.initIsChar && !isArray && node.children.empty()) || (!node.children.empty() && exprProducesString(node.children[0])));
            globalVarIsString[node.value] = isStr;
            globalVarIsArray[node.value] = isArray || node.isFixedArray;
            if (node.initUninitialized) {
                std::string c = node.isConst ? "const " : "";
                if (node.isFixedArray) {
                    std::string cppType = (node.declType == "unsigned char") ? "unsigned char" : (node.declType == "char") ? "char" : "int";
                    out << c << cppType << " " << vname << "[" << node.arraySize << "];\n";
                } else if (!node.declType.empty() && node.declType == "int") {
                    out << c << "int " << vname << " = 0;\n";
                } else if (!node.declType.empty() && node.declType == "bool") {
                    out << c << "bool " << vname << " = false;\n";
                } else if (!node.declType.empty() && node.declType == "float") {
                    out << c << "double " << vname << " = 0.0;\n";
                } else if (!node.declType.empty() && node.declType == "char") {
                    out << c << "char " << vname << " = '\\0';\n";
                } else {
                    out << c << "std::string " << vname << ";\n";
                }
            } else if (isArray && !node.children.empty()) {
                std::string c = node.isConst ? "const " : "";
                out << c << "std::vector<int> " << vname << " = " << emitExpr(node.children[0], globalVarMap, fnName) << ";\n";
            } else if (!node.children.empty()) {
                std::string c = node.isConst ? "const " : "";
                bool useBool = !node.declType.empty() ? (node.declType == "bool") : node.initIsBool;
                bool useInt = !node.declType.empty() ? (node.declType == "int") : node.initIsInt;
                bool useFloat = !node.declType.empty() ? (node.declType == "float") : node.initIsFloat;
                bool useChar = !node.declType.empty() ? (node.declType == "char") : node.initIsChar;
                std::string cppType = c + (useBool ? "bool " : useFloat ? "double " : useChar ? "char " : (useInt ? "int " : "std::string "));
                out << cppType << vname << " = " << emitExpr(node.children[0], globalVarMap, fnName) << ";\n";
            } else if (node.initIsBool || (!node.declType.empty() && node.declType == "bool")) {
                std::string c = node.isConst ? "const " : "";
                out << c << "bool " << vname << " = " << (node.initValue == "true" ? "true" : "false") << ";\n";
            } else if (node.initIsInt || (!node.declType.empty() && node.declType == "int")) {
                std::string c = node.isConst ? "const " : "";
                out << c << "int " << vname << " = " << node.initValue << ";\n";
            } else {
                std::string c = node.isConst ? "const " : "";
                out << c << "std::string " << vname << " = \"" << escapeString(node.initValue) << "\";\n";
            }
        }
        if (!globalVarMap.empty()) out << "\n";

        // Emit functions first (before main)
        for (const AstNode& node : ast_) {
            if (node.type == AstNode::Type::Function) {
                std::string cppName = fnName(node.value);
                bool hasReturn = blockHasReturn(node.children);
                std::string retType = hasReturn ? "int" : "void";
                out << (buildDll_ ? "extern \"C\" NEXA_EXPORT " : "static ") << retType << " " << cppName << "(";
                std::map<std::string, std::string> varMap = globalVarMap;
                int varIdx = 0;
                for (size_t i = 0; i < node.paramNames.size(); i++) {
                    if (i > 0) out << ", ";
                    std::string pname = preserveNames_ ? node.paramNames[i] : ("__nexa_param_" + std::to_string(i));
                    std::string ptype = "int";
                    if (i < node.paramTypes.size()) {
                        if (node.paramTypes[i] == "string") ptype = "std::string";
                        else if (node.paramTypes[i] == "bool") ptype = "bool";
                        else if (node.paramTypes[i] == "float") ptype = "double";
                        else if (node.paramTypes[i] == "char") ptype = "char";
                    }
                    out << ptype << " " << pname;
                    varMap[node.paramNames[i]] = pname;
                }
                out << ") {\n";
                varIdx = static_cast<int>(node.paramNames.size());
                std::map<std::string, bool> varIsString = globalVarIsString;
                std::map<std::string, bool> varIsConst = globalVarIsConst;
                std::map<std::string, bool> varIsFloat = globalVarIsFloat;
                std::map<std::string, bool> varIsChar = globalVarIsChar;
                for (size_t i = 0; i < node.paramNames.size(); i++) {
                    bool isStr = (i < node.paramTypes.size() && node.paramTypes[i] == "string");
                    varIsString[node.paramNames[i]] = isStr;
                    varIsFloat[node.paramNames[i]] = (i < node.paramTypes.size() && node.paramTypes[i] == "float");
                    varIsChar[node.paramNames[i]] = (i < node.paramTypes.size() && node.paramTypes[i] == "char");
                }
                emitBlock(out, node.children, fnName, varMap, varIdx, varIsString, varIsConst, varIsFloat, varIsChar);
                if (hasReturn && (node.children.empty() || node.children.back().type != AstNode::Type::Return)) {
                    out << "    return 0;\n";  // fallback if control reaches end
                }
                out << "}\n\n";
            }
        }

        // Emit DLL/SO loader hook: auto-call __init__ when library is loaded
        if (buildDll_) {
            bool hasInit = false;
            for (const AstNode& node : ast_) {
                if (node.type == AstNode::Type::Function && node.value == "__init__") {
                    hasInit = true;
                    break;
                }
            }
            if (hasInit) {
                for (const AstNode& node : ast_) {
                    if (node.type == AstNode::Type::Function && node.value == "__init__" && !node.paramNames.empty()) {
                        throw std::runtime_error("fn __init__() must have no parameters for DLL/SO auto-init");
                    }
                }
                std::string initName = fnName("__init__");
                out << "#ifdef _WIN32\n";
                out << "#include <windows.h>\n";
                out << "BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {\n";
                out << "    (void)hinstDLL;\n";
                out << "    (void)lpvReserved;\n";
                out << "    if (fdwReason == DLL_PROCESS_ATTACH) {\n";
                out << "        " << initName << "();\n";
                out << "    }\n";
                out << "    return TRUE;\n";
                out << "}\n";
                out << "#else\n";
                out << "__attribute__((constructor))\n";
                out << "static void __nexa_auto_init(void) {\n";
                out << "    " << initName << "();\n";
                out << "}\n";
                out << "#endif\n\n";
            }
        }

        // Emit main (skip for DLL)
        if (!buildDll_) {
        for (const AstNode& node : ast_) {
            if (node.type == AstNode::Type::MainFunction) {
                out << "int main() {\n";
                std::map<std::string, std::string> varMap = globalVarMap;
                int varIdx = 0;
                std::map<std::string, bool> varIsString = globalVarIsString;
                std::map<std::string, bool> varIsConst = globalVarIsConst;
                std::map<std::string, bool> varIsFloat = globalVarIsFloat;
                std::map<std::string, bool> varIsChar = globalVarIsChar;
                emitBlock(out, node.children, fnName, varMap, varIdx, varIsString, varIsConst, varIsFloat, varIsChar);
                out << "    return 0;\n";
                out << "}\n";
            }
        }
        }

        return out.str();
    }

private:
    const std::vector<AstNode>& ast_;
    const Modules& modules_;
    bool preserveNames_;
    bool buildDll_;

    using FnNameFn = std::function<std::string(const std::string&)>;

    static bool exprProducesString(const AstNode& e) {
        if (e.type == AstNode::Type::OsGetenv || e.type == AstNode::Type::OsPlatform || e.type == AstNode::Type::OsExeDir || e.type == AstNode::Type::ExprStringLiteral) return true;
        if (e.type == AstNode::Type::ExprAdd && e.children.size() >= 2) {
            return exprProducesString(e.children[0]) || exprProducesString(e.children[1]);
        }
        return false;
    }

    static bool exprIsString(const AstNode& e, const std::map<std::string, bool>& varIsString) {
        if (e.type == AstNode::Type::ExprVarRef) {
            auto it = varIsString.find(e.value);
            return it != varIsString.end() && it->second;
        }
        if (e.type == AstNode::Type::OsGetenv || e.type == AstNode::Type::OsPlatform || e.type == AstNode::Type::OsExeDir || e.type == AstNode::Type::ExprStringLiteral || e.type == AstNode::Type::FileRead || e.type == AstNode::Type::IoReadln) return true;
        if (e.type == AstNode::Type::ExprAdd && e.children.size() >= 2) {
            return exprIsString(e.children[0], varIsString) || exprIsString(e.children[1], varIsString);
        }
        return false;
    }

    static bool exprIsFloat(const AstNode& e, const std::map<std::string, bool>& varIsFloat) {
        if (e.type == AstNode::Type::ExprFloatLiteral) return true;
        if (e.type == AstNode::Type::ExprVarRef) {
            auto it = varIsFloat.find(e.value);
            return it != varIsFloat.end() && it->second;
        }
        if (e.type == AstNode::Type::ExprAdd || e.type == AstNode::Type::ExprSub || e.type == AstNode::Type::ExprMul ||
            e.type == AstNode::Type::ExprDiv || e.type == AstNode::Type::ExprMod) {
            if (e.children.size() >= 2)
                return exprIsFloat(e.children[0], varIsFloat) || exprIsFloat(e.children[1], varIsFloat);
        }
        return false;
    }

    static bool exprIsChar(const AstNode& e, const std::map<std::string, bool>& varIsChar) {
        if (e.type == AstNode::Type::ExprCharLiteral) return true;
        if (e.type == AstNode::Type::ExprVarRef) {
            auto it = varIsChar.find(e.value);
            return it != varIsChar.end() && it->second;
        }
        return false;
    }

    static bool blockHasReturn(const std::vector<AstNode>& children) {
        for (const AstNode& c : children) {
            if (c.type == AstNode::Type::Return) return true;
        }
        return false;
    }

    void emitBlock(std::ostringstream& out, const std::vector<AstNode>& children, const FnNameFn& fnName,
                   std::map<std::string, std::string>& varMap, int& varIdx,
                   std::map<std::string, bool>& varIsString, std::map<std::string, bool>& varIsConst,
                   std::map<std::string, bool>& varIsFloat, std::map<std::string, bool>& varIsChar,
                   const std::string& indent = "    ", bool inStringSwitchCase = false) {
        for (const AstNode& child : children) {
            if (child.type == AstNode::Type::Variable) {
                std::string vname = preserveNames_ ? child.value : ("__nexa_var_" + std::to_string(varIdx++));
                if (!preserveNames_) varMap[child.value] = vname;
                varIsConst[child.value] = child.isConst;
                bool isFloat = (!child.declType.empty() && child.declType == "float") || child.initIsFloat;
                bool isChar = (!child.declType.empty() && child.declType == "char") || child.initIsChar;
                varIsFloat[child.value] = isFloat;
                varIsChar[child.value] = isChar;
                bool isArray = child.initFromArray || (!child.children.empty() && child.children[0].type == AstNode::Type::ExprArrayLiteral);
                bool isStr = !isArray && (!child.declType.empty() ? (child.declType == "string") : (child.initUninitialized || child.initFromReadln || child.initFromFileRead || (!child.initIsInt && !child.initIsBool && !child.initIsFloat && !child.initIsChar && !child.initFromDllLoad && child.children.empty()) ||
                    (!child.children.empty() && exprProducesString(child.children[0]))));
                varIsString[child.value] = isStr;
                if (child.initFromReadln) {
                    out << indent << "char __nexa_buf[4096];\n";
                    out << indent << "if (fgets(__nexa_buf, sizeof(__nexa_buf), stdin)) { __nexa_buf[strcspn(__nexa_buf, \"\\n\")] = 0; }\n";
                    out << indent << "std::string " << vname << "(__nexa_buf);\n";
                } else if (child.initFromDllLoad) {
#ifdef _WIN32
                    out << indent << "__nexa_dll_handles.push_back((void*)LoadLibraryA(\"" << escapeString(child.initValue) << "\"));\n";
#else
                    out << indent << "__nexa_dll_handles.push_back(dlopen(\"" << escapeString(child.initValue) << "\", RTLD_LAZY));\n";
#endif
                    out << indent << "int " << vname << " = (int)__nexa_dll_handles.size() - 1;\n";
                } else if (!child.children.empty() && child.children[0].type == AstNode::Type::OsGetenv) {
                    const std::string& envName = child.children[0].value;
                    out << indent << "const char* __nexa_ge_" << varIdx << " = getenv(\"" << escapeString(envName) << "\");\n";
                    out << indent << "std::string " << vname << " = __nexa_ge_" << varIdx << " ? __nexa_ge_" << varIdx << " : \"\";\n";
                } else if (!child.children.empty() && child.children[0].type == AstNode::Type::OsPlatform) {
                    out << indent << "std::string " << vname << " = __nexa_os_platform();\n";
                } else if (!child.children.empty() && child.children[0].type == AstNode::Type::OsExeDir) {
                    out << indent << "std::string " << vname << " = __nexa_exe_dir();\n";
                } else if (child.initUninitialized) {
                    std::string c = child.isConst ? "const " : "";
                    if (child.isFixedArray) {
                        std::string cppType = (child.declType == "unsigned char") ? "unsigned char" : (child.declType == "char") ? "char" : "int";
                        out << indent << c << cppType << " " << vname << "[" << child.arraySize << "];\n";
                    } else if (!child.declType.empty() && child.declType == "int") {
                        out << indent << c << "int " << vname << " = 0;\n";
                    } else if (!child.declType.empty() && child.declType == "bool") {
                        out << indent << c << "bool " << vname << " = false;\n";
                    } else if (!child.declType.empty() && child.declType == "float") {
                        out << indent << c << "double " << vname << " = 0.0;\n";
                    } else if (!child.declType.empty() && child.declType == "char") {
                        out << indent << c << "char " << vname << " = '\\0';\n";
                    } else {
                        out << indent << c << "std::string " << vname << ";\n";
                    }
                } else if (child.initFromFileRead && !child.children.empty()) {
                    std::string pathExpr = emitExpr(child.children[0], varMap, fnName);
                    out << indent << "std::string " << vname << ";\n";
                    out << indent << "{\n";
                    out << indent << "    std::ifstream __f(" << pathExpr << ");\n";
                    out << indent << "    std::stringstream __ss; __ss << __f.rdbuf(); " << vname << " = __ss.str();\n";
                    out << indent << "}\n";
                } else if (isArray && !child.children.empty()) {
                    std::string c = child.isConst ? "const " : "";
                    out << indent << c << "std::vector<int> " << vname << " = " << emitExpr(child.children[0], varMap, fnName) << ";\n";
                } else if (!child.children.empty()) {
                    std::string c = child.isConst ? "const " : "";
                    bool useBool = !child.declType.empty() ? (child.declType == "bool") : child.initIsBool;
                    bool useInt = !child.declType.empty() ? (child.declType == "int") : child.initIsInt;
                    bool useFloat = !child.declType.empty() ? (child.declType == "float") : child.initIsFloat;
                    bool useChar = !child.declType.empty() ? (child.declType == "char") : child.initIsChar;
                    std::string cppType = c + (useBool ? "bool " : useFloat ? "double " : useChar ? "char " : (useInt ? "int " : "std::string "));
                    out << indent << cppType << vname << " = " << emitExpr(child.children[0], varMap, fnName) << ";\n";
                } else if (child.initIsBool || (!child.declType.empty() && child.declType == "bool")) {
                    std::string c = child.isConst ? "const " : "";
                    out << indent << c << "bool " << vname << " = " << (child.initValue == "true" ? "true" : "false") << ";\n";
                } else if (child.initIsInt || (!child.declType.empty() && child.declType == "int")) {
                    std::string c = child.isConst ? "const " : "";
                    out << indent << c << "int " << vname << " = " << child.initValue << ";\n";
                } else {
                    std::string c = child.isConst ? "const " : "";
                    out << indent << c << "std::string " << vname << " = \"" << escapeString(child.initValue) << "\";\n";
                }
            } else if (child.type == AstNode::Type::IoPrintln) {
                if (!child.children.empty()) {
                    bool exprIsStr = exprIsString(child.children[0], varIsString);
                    bool exprIsF = exprIsFloat(child.children[0], varIsFloat);
                    bool exprIsC = exprIsChar(child.children[0], varIsChar);
                    std::string expr = emitExpr(child.children[0], varMap, fnName);
                    std::string fmt = exprIsStr ? "%s" : exprIsF ? "%g" : exprIsC ? "%c" : "%d";
                    std::string arg = exprIsStr ? expr + ".c_str()" : expr;
                    out << indent << "printf(\"" << fmt << "\\n\", " << arg << ");\n";
                } else if (child.isVarRef) {
                    std::string v = preserveNames_ ? child.value : varMap.at(child.value);
                    bool isStr = varIsString.count(child.value) && varIsString.at(child.value);
                    bool isF = varIsFloat.count(child.value) && varIsFloat.at(child.value);
                    bool isC = varIsChar.count(child.value) && varIsChar.at(child.value);
                    std::string fmt = isStr ? "%s" : isF ? "%g" : isC ? "%c" : "%d";
                    std::string arg = isStr ? v + ".c_str()" : v;
                    out << indent << "printf(\"" << fmt << "\\n\", " << arg << ");\n";
                } else {
                    out << indent << "printf(\"" << escapeStringForPrintf(child.value) << "\\n\");\n";
                }
            } else if (child.type == AstNode::Type::IoPrint) {
                if (!child.children.empty()) {
                    bool exprIsStr = exprIsString(child.children[0], varIsString);
                    bool exprIsF = exprIsFloat(child.children[0], varIsFloat);
                    bool exprIsC = exprIsChar(child.children[0], varIsChar);
                    std::string expr = emitExpr(child.children[0], varMap, fnName);
                    std::string fmt = exprIsStr ? "%s" : exprIsF ? "%g" : exprIsC ? "%c" : "%d";
                    std::string arg = exprIsStr ? expr + ".c_str()" : expr;
                    out << indent << "printf(\"" << fmt << "\", " << arg << ");\n";
                } else if (child.isVarRef) {
                    std::string v = preserveNames_ ? child.value : varMap.at(child.value);
                    bool isStr = varIsString.count(child.value) && varIsString.at(child.value);
                    bool isF = varIsFloat.count(child.value) && varIsFloat.at(child.value);
                    bool isC = varIsChar.count(child.value) && varIsChar.at(child.value);
                    std::string fmt = isStr ? "%s" : isF ? "%g" : isC ? "%c" : "%d";
                    std::string arg = isStr ? v + ".c_str()" : v;
                    out << indent << "printf(\"" << fmt << "\", " << arg << ");\n";
                } else {
                    out << indent << "printf(\"" << escapeStringForPrintf(child.value) << "\");\n";
                }
            } else if (child.type == AstNode::Type::FileRead) {
                std::string pathExpr = emitExpr(child.children[0], varMap, fnName);
                out << indent << "{\n";
                out << indent << "    std::ifstream __f(" << pathExpr << ");\n";
                out << indent << "    std::stringstream __ss; __ss << __f.rdbuf();\n";
                out << indent << "}\n";
            } else if (child.type == AstNode::Type::FileWrite) {
                std::string pathExpr = emitExpr(child.children[0], varMap, fnName);
                std::string contentExpr = emitExpr(child.children[1], varMap, fnName);
                out << indent << "{\n";
                out << indent << "    std::ofstream __f(" << pathExpr << ");\n";
                out << indent << "    __f << " << contentExpr << ";\n";
                out << indent << "}\n";
            } else if (child.type == AstNode::Type::FileAppend) {
                std::string pathExpr = emitExpr(child.children[0], varMap, fnName);
                std::string contentExpr = emitExpr(child.children[1], varMap, fnName);
                out << indent << "{\n";
                out << indent << "    std::ofstream __f(" << pathExpr << ", std::ios::app);\n";
                out << indent << "    __f << " << contentExpr << ";\n";
                out << indent << "}\n";
            } else if (child.type == AstNode::Type::FileExists) {
                std::string pathExpr = emitExpr(child.children[0], varMap, fnName);
                out << indent << "(void)(std::filesystem::exists(" << pathExpr << ") ? 1 : 0);\n";
            } else if (child.type == AstNode::Type::RandomSeed) {
                std::string seedExpr = emitExpr(child.children[0], varMap, fnName);
                out << indent << "__nexa_random_seed(" << seedExpr << ");\n";
            } else if (child.type == AstNode::Type::RandomInt) {
                std::string minExpr = emitExpr(child.children[0], varMap, fnName);
                std::string maxExpr = emitExpr(child.children[1], varMap, fnName);
                out << indent << "(void)__nexa_random_int(" << minExpr << ", " << maxExpr << ");\n";
            } else if (child.type == AstNode::Type::TimeSleep) {
                std::string msExpr = emitExpr(child.children[0], varMap, fnName);
                out << indent << "std::this_thread::sleep_for(std::chrono::milliseconds(" << msExpr << "));\n";
            } else if (child.type == AstNode::Type::DllCall) {
                std::string h = preserveNames_ ? child.children[0].value : varMap.at(child.children[0].value);
#ifdef _WIN32
                out << indent << "{\n";
                out << indent << "    void (*fn)() = (void(*)())GetProcAddress((HMODULE)__nexa_dll_handles[" << h << "], \"" << escapeString(child.value) << "\");\n";
                out << indent << "    if (fn) fn();\n";
                out << indent << "}\n";
#else
                out << indent << "{\n";
                out << indent << "    void (*fn)() = (void(*)())dlsym(__nexa_dll_handles[" << h << "], \"" << escapeString(child.value) << "\");\n";
                out << indent << "    if (fn) fn();\n";
                out << indent << "}\n";
#endif
            } else if (child.type == AstNode::Type::OsSystem) {
                if (!child.children.empty()) {
                    bool exprIsStr = exprIsString(child.children[0], varIsString);
                    std::string expr = emitExpr(child.children[0], varMap, fnName);
                    out << indent << "std::system(" << (exprIsStr ? expr + ".c_str()" : expr) << ");\n";
                } else if (child.isVarRef) {
                    std::string v = preserveNames_ ? child.value : varMap.at(child.value);
                    out << indent << "std::system(" << v << ".c_str());\n";
                } else {
                    out << indent << "std::system(\"" << escapeString(child.value) << "\");\n";
                }
            } else if (child.type == AstNode::Type::FnCall) {
                out << indent << fnName(child.value) << "(";
                for (size_t i = 0; i < child.children.size(); i++) {
                    if (i > 0) out << ", ";
                    out << emitExpr(child.children[i], varMap, fnName);
                }
                out << ");\n";
            } else if (child.type == AstNode::Type::Assignment) {
                if (varIsConst.count(child.value) && varIsConst[child.value]) {
                    throw std::runtime_error("Cannot assign to const variable '" + child.value + "'");
                }
                std::string v = preserveNames_ ? child.value : varMap.at(child.value);
                out << indent << v << " = " << emitExpr(child.children[0], varMap, fnName) << ";\n";
            } else if (child.type == AstNode::Type::AssnIndex) {
                if (varIsConst.count(child.value) && varIsConst[child.value]) {
                    throw std::runtime_error("Cannot assign to const variable '" + child.value + "'");
                }
                std::string v = preserveNames_ ? child.value : varMap.at(child.value);
                std::string idx = emitExpr(child.children[0], varMap, fnName);
                std::string val = emitExpr(child.children[1], varMap, fnName);
                out << indent << v << "[" << idx << "] = " << val << ";\n";
            } else if (child.type == AstNode::Type::AssnAdd) {
                if (varIsConst.count(child.value) && varIsConst[child.value]) {
                    throw std::runtime_error("Cannot assign to const variable '" + child.value + "'");
                }
                std::string v = preserveNames_ ? child.value : varMap.at(child.value);
                out << indent << v << " = " << v << " + " << emitExpr(child.children[0], varMap, fnName) << ";\n";
            } else if (child.type == AstNode::Type::AssnSub) {
                if (varIsConst.count(child.value) && varIsConst[child.value]) {
                    throw std::runtime_error("Cannot assign to const variable '" + child.value + "'");
                }
                std::string v = preserveNames_ ? child.value : varMap.at(child.value);
                out << indent << v << " = " << v << " - " << emitExpr(child.children[0], varMap, fnName) << ";\n";
            } else if (child.type == AstNode::Type::AssnMul) {
                if (varIsConst.count(child.value) && varIsConst[child.value]) {
                    throw std::runtime_error("Cannot assign to const variable '" + child.value + "'");
                }
                std::string v = preserveNames_ ? child.value : varMap.at(child.value);
                out << indent << v << " = " << v << " * " << emitExpr(child.children[0], varMap, fnName) << ";\n";
            } else if (child.type == AstNode::Type::AssnDiv) {
                if (varIsConst.count(child.value) && varIsConst[child.value]) {
                    throw std::runtime_error("Cannot assign to const variable '" + child.value + "'");
                }
                std::string v = preserveNames_ ? child.value : varMap.at(child.value);
                out << indent << v << " = " << v << " / " << emitExpr(child.children[0], varMap, fnName) << ";\n";
            } else if (child.type == AstNode::Type::AssnMod) {
                if (varIsConst.count(child.value) && varIsConst[child.value]) {
                    throw std::runtime_error("Cannot assign to const variable '" + child.value + "'");
                }
                std::string v = preserveNames_ ? child.value : varMap.at(child.value);
                out << indent << v << " = " << v << " % " << emitExpr(child.children[0], varMap, fnName) << ";\n";
            } else if (child.type == AstNode::Type::IfElse) {
                emitIfElse(out, child, fnName, varMap, varIdx, varIsString, varIsConst, varIsFloat, varIsChar, indent, inStringSwitchCase);
            } else if (child.type == AstNode::Type::Switch) {
                emitSwitch(out, child, fnName, varMap, varIdx, varIsString, varIsConst, varIsFloat, varIsChar, indent, inStringSwitchCase);
            } else if (child.type == AstNode::Type::While) {
                std::string cond = emitCond(child.children[0], varMap, fnName);
                out << indent << "while (" << cond << ") {\n";
                emitBlock(out, child.children[1].children, fnName, varMap, varIdx, varIsString, varIsConst, varIsFloat, varIsChar, indent + "    ", inStringSwitchCase);
                out << indent << "}\n";
            } else if (child.type == AstNode::Type::For) {
                std::string loopVar = preserveNames_ ? child.value : ("__nexa_for_" + std::to_string(varIdx++));
                auto it = varMap.find(child.value);
                std::string prevVal = (it != varMap.end()) ? it->second : "";
                bool prevStr = varIsString.count(child.value) ? varIsString[child.value] : false;
                bool prevConst = varIsConst.count(child.value) ? varIsConst[child.value] : false;
                bool prevFloat = varIsFloat.count(child.value) ? varIsFloat[child.value] : false;
                bool prevChar = varIsChar.count(child.value) ? varIsChar[child.value] : false;
                varMap[child.value] = loopVar;
                varIsString[child.value] = false;
                varIsConst[child.value] = false;
                varIsFloat[child.value] = false;
                varIsChar[child.value] = false;
                std::string countExpr = emitExpr(child.children[0], varMap, fnName);
                out << indent << "for (int " << loopVar << " = 0; " << loopVar << " < " << countExpr << "; " << loopVar << "++) {\n";
                emitBlock(out, child.children[1].children, fnName, varMap, varIdx, varIsString, varIsConst, varIsFloat, varIsChar, indent + "    ", inStringSwitchCase);
                out << indent << "}\n";
                if (!prevVal.empty()) { varMap[child.value] = prevVal; varIsString[child.value] = prevStr; varIsConst[child.value] = prevConst; varIsFloat[child.value] = prevFloat; varIsChar[child.value] = prevChar; }
                else { varMap.erase(child.value); varIsString.erase(child.value); varIsConst.erase(child.value); varIsFloat.erase(child.value); varIsChar.erase(child.value); }
            } else if (child.type == AstNode::Type::Return) {
                out << indent << "return " << emitExpr(child.children[0], varMap, fnName) << ";\n";
            } else if (child.type == AstNode::Type::Break) {
                if (!inStringSwitchCase) out << indent << "break;\n";
            } else if (child.type == AstNode::Type::Continue) {
                if (!inStringSwitchCase) out << indent << "continue;\n";
            } else if (child.type == AstNode::Type::IncPost) {
                if (varIsConst.count(child.value) && varIsConst[child.value]) {
                    throw std::runtime_error("Cannot assign to const variable '" + child.value + "'");
                }
                std::string v = preserveNames_ ? child.value : varMap.at(child.value);
                out << indent << v << " = " << v << " + 1;\n";
            } else if (child.type == AstNode::Type::DecPost) {
                if (varIsConst.count(child.value) && varIsConst[child.value]) {
                    throw std::runtime_error("Cannot assign to const variable '" + child.value + "'");
                }
                std::string v = preserveNames_ ? child.value : varMap.at(child.value);
                out << indent << v << " = " << v << " - 1;\n";
            }
        }
    }

    void emitIfElse(std::ostringstream& out, const AstNode& node, const FnNameFn& fnName,
                   std::map<std::string, std::string>& varMap, int& varIdx,
                   std::map<std::string, bool>& varIsString, std::map<std::string, bool>& varIsConst,
                   std::map<std::string, bool>& varIsFloat, std::map<std::string, bool>& varIsChar,
                   const std::string& indent, bool inStringSwitchCase = false) {
        std::string cond = emitCond(node.children[0], varMap, fnName);
        out << indent << "if (" << cond << ") {\n";
        emitBlock(out, node.children[1].children, fnName, varMap, varIdx, varIsString, varIsConst, varIsFloat, varIsChar, indent + "    ", inStringSwitchCase);
        out << indent << "}";
        if (node.children.size() > 2) {
            const AstNode& elsePart = node.children[2];
            if (elsePart.type == AstNode::Type::IfElse) {
                out << " else if (" << emitCond(elsePart.children[0], varMap, fnName) << ") {\n";
                emitBlock(out, elsePart.children[1].children, fnName, varMap, varIdx, varIsString, varIsConst, varIsFloat, varIsChar, indent + "    ", inStringSwitchCase);
                out << indent << "}";
                if (elsePart.children.size() > 2) {
                    emitIfElseTail(out, elsePart.children[2], fnName, varMap, varIdx, varIsString, varIsConst, varIsFloat, varIsChar, indent, inStringSwitchCase);
                }
            } else {
                out << " else {\n";
                emitBlock(out, elsePart.children, fnName, varMap, varIdx, varIsString, varIsConst, varIsFloat, varIsChar, indent + "    ", inStringSwitchCase);
                out << indent << "}";
            }
        }
        out << "\n";
    }

    void emitSwitch(std::ostringstream& out, const AstNode& node, const FnNameFn& fnName,
                   std::map<std::string, std::string>& varMap, int& varIdx,
                   std::map<std::string, bool>& varIsString, std::map<std::string, bool>& varIsConst,
                   std::map<std::string, bool>& varIsFloat, std::map<std::string, bool>& varIsChar,
                   const std::string& indent, bool inStringSwitchCase = false) {
        std::string expr = emitExpr(node.children[0], varMap, fnName);
        bool useStringSwitch = false;
        for (size_t i = 1; i < node.children.size(); i++) {
            if (node.children[i].type == AstNode::Type::SwitchCase && node.children[i].caseIsString) {
                useStringSwitch = true;
                break;
            }
        }
        if (useStringSwitch) {
            std::vector<const AstNode*> cases, defaults;
            for (size_t i = 1; i < node.children.size(); i++) {
                const AstNode& c = node.children[i];
                if (c.type != AstNode::Type::SwitchCase) continue;
                if (c.value == "default") defaults.push_back(&c);
                else cases.push_back(&c);
            }
            bool first = true;
            for (const AstNode* c : cases) {
                out << indent << (first ? "" : "else ") << "if (" << expr << " == std::string(\"" << escapeString(c->initValue) << "\")) {\n";
                first = false;
                emitBlock(out, c->children, fnName, varMap, varIdx, varIsString, varIsConst, varIsFloat, varIsChar, indent + "    ", true);
                out << indent << "}\n";
            }
            for (const AstNode* c : defaults) {
                out << indent << (first ? "" : "else ") << "{\n";
                first = false;
                emitBlock(out, c->children, fnName, varMap, varIdx, varIsString, varIsConst, varIsFloat, varIsChar, indent + "    ", true);
                out << indent << "}\n";
            }
        } else {
            out << indent << "switch (" << expr << ") {\n";
            for (size_t i = 1; i < node.children.size(); i++) {
                const AstNode& c = node.children[i];
                if (c.type != AstNode::Type::SwitchCase) continue;
                if (c.value == "default") {
                    out << indent << "default:\n";
                } else {
                    out << indent << "case " << c.value << ":\n";
                }
                emitBlock(out, c.children, fnName, varMap, varIdx, varIsString, varIsConst, varIsFloat, varIsChar, indent + "    ");
            }
            out << indent << "}\n";
        }
    }

    void emitIfElseTail(std::ostringstream& out, const AstNode& part, const FnNameFn& fnName,
                       std::map<std::string, std::string>& varMap, int& varIdx,
                       std::map<std::string, bool>& varIsString, std::map<std::string, bool>& varIsConst,
                       std::map<std::string, bool>& varIsFloat, std::map<std::string, bool>& varIsChar,
                       const std::string& indent, bool inStringSwitchCase = false) {
        if (part.type == AstNode::Type::IfElse) {
            out << " else if (" << emitCond(part.children[0], varMap, fnName) << ") {\n";
            emitBlock(out, part.children[1].children, fnName, varMap, varIdx, varIsString, varIsConst, varIsFloat, varIsChar, indent + "    ", inStringSwitchCase);
            out << indent << "}";
            if (part.children.size() > 2) {
                emitIfElseTail(out, part.children[2], fnName, varMap, varIdx, varIsString, varIsConst, varIsFloat, varIsChar, indent, inStringSwitchCase);
            }
        } else {
            out << " else {\n";
            emitBlock(out, part.children, fnName, varMap, varIdx, varIsString, varIsConst, varIsFloat, varIsChar, indent + "    ", inStringSwitchCase);
            out << indent << "}";
        }
    }

    std::string emitCond(const AstNode& c, const std::map<std::string, std::string>& varMap, const FnNameFn& fnName) {
        switch (c.type) {
            case AstNode::Type::CondEq:
                return "(" + emitExpr(c.children[0], varMap, fnName) + " == " + emitExpr(c.children[1], varMap, fnName) + ")";
            case AstNode::Type::CondNe:
                return "(" + emitExpr(c.children[0], varMap, fnName) + " != " + emitExpr(c.children[1], varMap, fnName) + ")";
            case AstNode::Type::CondLt:
                return "(" + emitExpr(c.children[0], varMap, fnName) + " < " + emitExpr(c.children[1], varMap, fnName) + ")";
            case AstNode::Type::CondLe:
                return "(" + emitExpr(c.children[0], varMap, fnName) + " <= " + emitExpr(c.children[1], varMap, fnName) + ")";
            case AstNode::Type::CondGt:
                return "(" + emitExpr(c.children[0], varMap, fnName) + " > " + emitExpr(c.children[1], varMap, fnName) + ")";
            case AstNode::Type::CondGe:
                return "(" + emitExpr(c.children[0], varMap, fnName) + " >= " + emitExpr(c.children[1], varMap, fnName) + ")";
            case AstNode::Type::CondAnd:
                return "(" + emitCond(c.children[0], varMap, fnName) + " && " + emitCond(c.children[1], varMap, fnName) + ")";
            case AstNode::Type::CondOr:
                return "(" + emitCond(c.children[0], varMap, fnName) + " || " + emitCond(c.children[1], varMap, fnName) + ")";
            case AstNode::Type::CondNot:
                return "!(" + emitCond(c.children[0], varMap, fnName) + ")";
            case AstNode::Type::ExprBoolLiteral:
                return c.value;
            case AstNode::Type::ExprIntLiteral:
            case AstNode::Type::ExprFloatLiteral:
            case AstNode::Type::ExprCharLiteral:
            case AstNode::Type::ExprVarRef:
                return emitExpr(c, varMap, fnName);
            case AstNode::Type::FileExists: {
                std::string pathExpr = emitExpr(c.children[0], varMap, fnName);
                return "std::filesystem::exists(" + pathExpr + ")";
            }
            default:
                return "false";
        }
    }

    std::string emitExpr(const AstNode& e, const std::map<std::string, std::string>& varMap, const FnNameFn& fnName) {
        switch (e.type) {
            case AstNode::Type::ExprIntLiteral:
                return e.value;
            case AstNode::Type::ExprFloatLiteral:
                return e.value;
            case AstNode::Type::ExprCharLiteral: {
                if (e.value.empty()) return "'\\0'";
                unsigned char c = static_cast<unsigned char>(e.value[0]);
                if (c == '\'') return "'\\''";
                if (c == '\\') return "'\\\\'";
                if (c == '\n') return "'\\n'";
                if (c == '\t') return "'\\t'";
                if (c == '\r') return "'\\r'";
                if (c < 32 || c == 127) {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "'\\x%02X'", c);
                    return std::string(buf);
                }
                return "'" + std::string(1, c) + "'";
            }
            case AstNode::Type::ExprBoolLiteral:
                return e.value;
            case AstNode::Type::ExprStringLiteral:
                return "std::string(\"" + escapeString(e.value) + "\")";
            case AstNode::Type::OsGetenv: {
                std::string s = "([]{ const char* __p = getenv(\"" + escapeString(e.value) + "\"); return __p ? std::string(__p) : std::string(\"\"); }())";
                return s;
            }
            case AstNode::Type::OsPlatform:
                return "__nexa_os_platform()";
            case AstNode::Type::OsExeDir:
                return "__nexa_exe_dir()";
            case AstNode::Type::IoReadln: {
                return "([]{ char __b[4096]; if (fgets(__b, sizeof(__b), stdin)) __b[strcspn(__b, \"\\n\")] = 0; return std::string(__b); }())";
            }
            case AstNode::Type::IoToInt: {
                std::string s = emitExpr(e.children[0], varMap, fnName);
                return "__nexa_to_int(" + s + ")";
            }
            case AstNode::Type::FileRead: {
                std::string pathExpr = emitExpr(e.children[0], varMap, fnName);
                return "([]{ std::ifstream __f(" + pathExpr + "); std::stringstream __ss; __ss << __f.rdbuf(); return __ss.str(); }())";
            }
            case AstNode::Type::FileExists: {
                std::string pathExpr = emitExpr(e.children[0], varMap, fnName);
                return "(std::filesystem::exists(" + pathExpr + ") ? 1 : 0)";
            }
            case AstNode::Type::RandomInt: {
                std::string minExpr = emitExpr(e.children[0], varMap, fnName);
                std::string maxExpr = emitExpr(e.children[1], varMap, fnName);
                return "__nexa_random_int(" + minExpr + ", " + maxExpr + ")";
            }
            case AstNode::Type::TimeSeconds: {
                std::string n = emitExpr(e.children[0], varMap, fnName);
                return "(" + n + " * 1000)";
            }
            case AstNode::Type::TimeMilliseconds: {
                return emitExpr(e.children[0], varMap, fnName);
            }
            case AstNode::Type::ExprVarRef: {
                auto it = varMap.find(e.value);
                return (it != varMap.end()) ? it->second : e.value;
            }
            case AstNode::Type::ExprArrayLiteral: {
                std::string s = "std::vector<int>{";
                for (size_t i = 0; i < e.children.size(); i++) {
                    if (i > 0) s += ", ";
                    s += emitExpr(e.children[i], varMap, fnName);
                }
                s += "}";
                return s;
            }
            case AstNode::Type::ExprArrayIndex: {
                auto it = varMap.find(e.value);
                std::string v = (it != varMap.end()) ? it->second : e.value;
                return v + "[" + emitExpr(e.children[0], varMap, fnName) + "]";
            }
            case AstNode::Type::FnCall: {
                std::string s = fnName(e.value) + "(";
                for (size_t i = 0; i < e.children.size(); i++) {
                    if (i > 0) s += ", ";
                    s += emitExpr(e.children[i], varMap, fnName);
                }
                s += ")";
                return s;
            }
            case AstNode::Type::ExprAdd:
                return "(" + emitExpr(e.children[0], varMap, fnName) + " + " + emitExpr(e.children[1], varMap, fnName) + ")";
            case AstNode::Type::ExprSub:
                return "(" + emitExpr(e.children[0], varMap, fnName) + " - " + emitExpr(e.children[1], varMap, fnName) + ")";
            case AstNode::Type::ExprMul:
                return "(" + emitExpr(e.children[0], varMap, fnName) + " * " + emitExpr(e.children[1], varMap, fnName) + ")";
            case AstNode::Type::ExprDiv:
                return "(" + emitExpr(e.children[0], varMap, fnName) + " / " + emitExpr(e.children[1], varMap, fnName) + ")";
            case AstNode::Type::ExprMod:
                return "(" + emitExpr(e.children[0], varMap, fnName) + " % " + emitExpr(e.children[1], varMap, fnName) + ")";
            default:
                return "0";
        }
    }

    std::string escapeStringForPrintf(const std::string& s) {
        std::string out;
        for (unsigned char c : s) {
            if (c == '\\') out += "\\\\";
            else if (c == '"') out += "\\\"";
            else if (c == '%') out += "%%";
            else if (c == '\n') out += "\\n";
            else if (c == '\t') out += "\\t";
            else if (c == '\r') out += "\\r";
            else if (c < 32 || c == 127) {
                char buf[5];
                snprintf(buf, sizeof(buf), "\\x%02X", c);
                out += buf;
            }
            else out += c;
        }
        return out;
    }

    std::string escapeString(const std::string& s) {
        std::string out;
        for (unsigned char c : s) {
            if (c == '\\') out += "\\\\";
            else if (c == '"') out += "\\\"";
            else if (c == '\n') out += "\\n";
            else if (c == '\t') out += "\\t";
            else if (c == '\r') out += "\\r";
            else if (c < 32 || c == 127) {
                char buf[5];
                snprintf(buf, sizeof(buf), "\\x%02X", c);
                out += buf;
            }
            else out += c;
        }
        return out;
    }
};

}  // namespace nexa
