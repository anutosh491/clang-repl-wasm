#include "llvm/Support/raw_ostream.h"
#include <clang/Basic/TargetOptions.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Interpreter/Interpreter.h>

#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>

#include <iostream>

#include <dlfcn.h>
#include <unistd.h>

#include <emscripten.h>

llvm::SmallString<1024> OutputModule;
std::unique_ptr<clang::Interpreter> Interp;

extern "C" EMSCRIPTEN_KEEPALIVE void *get_last_module_addr() {
  return OutputModule.data();
}
extern "C" EMSCRIPTEN_KEEPALIVE uint32_t get_last_module_size() {
  return OutputModule.size_in_bytes();
}

int module_count = 0;

extern "C" EMSCRIPTEN_KEEPALIVE int init() {
  LLVMInitializeWebAssemblyTargetInfo();
  LLVMInitializeWebAssemblyTarget();
  LLVMInitializeWebAssemblyTargetMC();
  LLVMInitializeWebAssemblyAsmPrinter();

  clang::IncrementalCompilerBuilder CB;
  CB.SetCompilerArgs({"-target", "wasm32-unknown-emscripten", "-pie", "-shared",
                      "-std=c++17", "-xc++", "-resource-dir", "/lib/clang/19", "-v"});
  auto CI = CB.CreateCpp();
  if (!CI) {
    std::cerr << "Failed to create compiler instance!" << std::endl;
    return -1;
  }

  assert((*CI)->hasDiagnostics() && "1 Compiler has no diagnostics !!");

  auto Interpreter = clang::Interpreter::create(std::move(*CI));
  if (!Interpreter) {
    std::cerr << "Failed to create interpreter!" << std::endl;
    return -2;
  }

  Interp = std::move(*Interpreter);

  return 0;
}

clang::PartialTranslationUnit *currentPTU;

extern "C" EMSCRIPTEN_KEEPALIVE int parse(const char *code) {
  auto PTU = Interp->Parse(code);
  if (!PTU) {
    std::cerr << "Failed to generate PTU" << std::endl;
    return -1;
  }
  currentPTU = &*PTU;

  return 0;
}
extern "C" EMSCRIPTEN_KEEPALIVE int execute() {
  auto error = Interp->Execute(*currentPTU);
  if (error) {
    std::cerr << "Failed to execute PTU" << std::endl;

    currentPTU->TheModule->print(llvm::errs(), nullptr);
    return -2;
  }

  return 0;
}
