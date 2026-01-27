#include <memory>
#include <optional>
#include <string>
#include <iostream>

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/LegacyPassManager.h"

#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"

using namespace llvm;

int main(int argc, char** argv) {
    // === Initialize target ===
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
    InitializeNativeTargetAsmParser();

    LLVMContext context;
    auto module = std::make_unique<Module>("nexa_module", context);
    IRBuilder<> builder(context);

    // === Simple int main() { return 0; } ===
    FunctionType *funcType = FunctionType::get(builder.getInt32Ty(), false);
    Function *mainFunc = Function::Create(funcType, Function::ExternalLinkage, "main", module.get());
    BasicBlock *entry = BasicBlock::Create(context, "entry", mainFunc);
    builder.SetInsertPoint(entry);
    builder.CreateRet(builder.getInt32(0));

    if (verifyModule(*module, &errs())) {
        errs() << "Module verification failed!\n";
        return 1;
    }

    // === Setup target machine ===
    std::string targetTriple = "x86_64-pc-linux-gnu"; // hardcoded for Linux x86_64
    module->setTargetTriple(targetTriple);

    std::string error;
    const Target *target = TargetRegistry::lookupTarget(targetTriple, error);
    if (!target) {
        errs() << "Target lookup failed: " << error << "\n";
        return 1;
    }

    TargetOptions opt;
    std::optional<Reloc::Model> RM;
    auto targetMachine = target->createTargetMachine(targetTriple, "generic", "", opt, RM);

    module->setDataLayout(targetMachine->createDataLayout());

    // === Emit object file ===
    std::error_code EC;
    raw_fd_ostream dest("output.o", EC, sys::fs::OF_None);
    if (EC) {
        errs() << "Could not open output.o for writing\n";
        return 1;
    }

    legacy::PassManager pass;
    if (targetMachine->addPassesToEmitFile(pass, dest, nullptr, llvm::CodeGenFileType::ObjectFile)) {
        errs() << "TargetMachine can't emit object file\n";
        return 1;
    }

    pass.run(*module);
    dest.flush();

    outs() << "Successfully generated output.o\n";
    return 0;
}
