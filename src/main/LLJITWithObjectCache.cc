//===--- LLJITWithObjectCache.cpp - An LLJIT example with an ObjectCache --===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include <llvm/Analysis/Passes.h>
#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/ObjectCache.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Linker/Linker.h>
#include <llvm/MC/SubtargetFeature.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Passes/PassPlugin.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/IPO/GlobalOpt.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Scalar/GVN.h>
#include <llvm/Transforms/Scalar/NewGVN.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
#include <llvm/Transforms/Utils.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/Mem2Reg.h>
#include <llvm/Transforms/Vectorize.h>
#include <llvm/Transforms/Vectorize/LoopVectorize.h>
#include <llvm/Transforms/Vectorize/SLPVectorizer.h>

#include "ExampleModules.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ExecutionEngine/ObjectCache.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Host.h"

using namespace llvm;
using namespace llvm::orc;

ExitOnError ExitOnErr;

extern const void* force_rtti_simple_compiler = typeid(llvm::orc::SimpleCompiler).name();
extern const void* force_rtti_object_cache = typeid(llvm::ObjectCache).name();
using namespace llvm;
using namespace llvm::orc;

// 通过定义一个子类，强制编译器生成 vtable 和 typeinfo
namespace {

class _ForceObjectCache : public llvm::ObjectCache {
public:
    void notifyObjectCompiled(const Module* M, MemoryBufferRef Obj) override {}
    std::unique_ptr<MemoryBuffer> getObject(const Module* M) override { return nullptr; }
};

class _ForceSimpleCompiler : public llvm::orc::SimpleCompiler {
public:
    using SimpleCompiler::SimpleCompiler;
};

} // namespace

class MyObjectCache : public ObjectCache {
public:
    void notifyObjectCompiled(const Module* M, MemoryBufferRef ObjBuffer) override {
        CachedObjects[M->getModuleIdentifier()] =
                MemoryBuffer::getMemBufferCopy(ObjBuffer.getBuffer(), ObjBuffer.getBufferIdentifier());
    }

    std::unique_ptr<MemoryBuffer> getObject(const Module* M) override {
        auto I = CachedObjects.find(M->getModuleIdentifier());
        if (I == CachedObjects.end()) {
            dbgs() << "No object for " << M->getModuleIdentifier() << " in cache. Compiling.\n";
            return nullptr;
        }

        dbgs() << "Object for " << M->getModuleIdentifier() << " loaded from cache.\n";
        return MemoryBuffer::getMemBuffer(I->second->getMemBufferRef());
    }

    void anchor() override {}

private:
    StringMap<std::unique_ptr<MemoryBuffer>> CachedObjects;
};
typedef int (*AddFunctionT)(int);
Expected<llvm::orc::JITTargetMachineBuilder> make_target_machine_builder() {
    llvm::orc::JITTargetMachineBuilder jtmb((llvm::Triple(llvm::sys::getDefaultTargetTriple())));
    auto const opt_level = llvm::CodeGenOpt::Aggressive; // or llvm::CodeGenOpt::None;
    jtmb.setCodeGenOptLevel(opt_level);
    return jtmb;
}

template <typename T>
T&& as_JIT_result(llvm::Expected<T>& expected, const std::string& error_context) {
    if (!expected) {
        outs() << (error_context + llvm::toString(expected.takeError()));
        abort();
    }
    return std::move(expected.get());
}

void use_JIT_link(llvm::orc::LLJITBuilder& jit_builder) {
    auto maybe_mem_manager = llvm::jitlink::InProcessMemoryManager::Create();
    auto mem_manager = as_JIT_result(maybe_mem_manager, "Could not create memory manager: ");
    jit_builder.setObjectLinkingLayerCreator([&](llvm::orc::ExecutionSession& ES, const llvm::Triple& TT) {
        return std::make_unique<llvm::orc::ObjectLinkingLayer>(ES, *mem_manager);
    });
}

static void optimize_module(llvm::Module& module, llvm::TargetIRAnalysis target_analysis) {
    // Setup an optimiser pipeline
    llvm::PassBuilder pass_builder;
    llvm::LoopAnalysisManager loop_am;
    llvm::FunctionAnalysisManager function_am;
    llvm::CGSCCAnalysisManager cgscc_am;
    llvm::ModuleAnalysisManager module_am;

    function_am.registerPass([&] { return target_analysis; });

    // Register required analysis managers
    pass_builder.registerModuleAnalyses(module_am);
    pass_builder.registerCGSCCAnalyses(cgscc_am);
    pass_builder.registerFunctionAnalyses(function_am);
    pass_builder.registerLoopAnalyses(loop_am);
    pass_builder.crossRegisterProxies(loop_am, function_am, cgscc_am, module_am);

    pass_builder.registerPipelineStartEPCallback(
            [&](llvm::ModulePassManager& module_pm, llvm::OptimizationLevel Level) {
                module_pm.addPass(llvm::ModuleInlinerPass());

                llvm::FunctionPassManager function_pm;
                function_pm.addPass(llvm::InstCombinePass());
                function_pm.addPass(llvm::PromotePass());
                function_pm.addPass(llvm::GVNPass());
                function_pm.addPass(llvm::NewGVNPass());
                function_pm.addPass(llvm::SimplifyCFGPass());
                function_pm.addPass(llvm::LoopVectorizePass());
                function_pm.addPass(llvm::SLPVectorizerPass());
                module_pm.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(function_pm)));

                module_pm.addPass(llvm::GlobalOptPass());
            });

    pass_builder.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O3).run(module, module_am);
}

void add_process_symbol(llvm::orc::LLJIT& lljit) {
    lljit.getMainJITDylib().addGenerator(llvm::cantFail(
            llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(lljit.getDataLayout().getGlobalPrefix())));
    // the `atexit` symbol cannot be found for ASAN
#ifdef ADDRESS_SANITIZER
    if (!lljit.lookup("atexit")) {
        add_absolute_symbol(lljit, "atexit", reinterpret_cast<void*>(atexit));
    }
#endif
}

Expected<std::unique_ptr<llvm::orc::LLJIT>> build_JIT(llvm::orc::JITTargetMachineBuilder jtmb,
                                                      std::reference_wrapper<ObjectCache>& object_cache) {
    llvm::orc::LLJITBuilder jit_builder;
    use_JIT_link(jit_builder);
    jit_builder.setJITTargetMachineBuilder(std::move(jtmb));
    jit_builder.setCompileFunctionCreator(
            [&object_cache](llvm::orc::JITTargetMachineBuilder JTMB)
                    -> llvm::Expected<std::unique_ptr<llvm::orc::IRCompileLayer::IRCompiler>> {
                auto target_machine = JTMB.createTargetMachine();
                if (!target_machine) {
                    return target_machine.takeError();
                }
                // after compilation, the object code will be stored into the given object cache
                return std::make_unique<llvm::orc::TMOwningSimpleCompiler>(std::move(*target_machine),
                                                                           &object_cache.get());
            });

    auto maybe_jit = jit_builder.create();
    auto jit = as_JIT_result(maybe_jit, "Could not create LLJIT instance: ");
    add_process_symbol(*jit);
    return std::move(jit);
}

AddFunctionT runJITWithCache(ObjectCache& ObjCache) {
    std::unique_ptr<llvm::LLVMContext> context;
    std::unique_ptr<llvm::orc::LLJIT> lljit;
    std::unique_ptr<llvm::IRBuilder<>> ir_builder;
    std::unique_ptr<llvm::Module> module;
    std::unique_ptr<llvm::TargetMachine> target_machine;

    auto tm_builder = ExitOnErr(make_target_machine_builder());
    std::reference_wrapper<ObjectCache> objCacheRef = std::ref(ObjCache);
    lljit = ExitOnErr(build_JIT(tm_builder, objCacheRef));

    target_machine = ExitOnErr(tm_builder.createTargetMachine());

    context = std::make_unique<LLVMContext>();

    SMDiagnostic Err;
    module = parseIR(MemoryBufferRef(Add1Example, "add1"), Err, *context);
    auto target_analysis = target_machine->getTargetIRAnalysis();
    optimize_module(*module, std::move(target_analysis));
    std::string error;

    llvm::raw_string_ostream errs(error);
    if (llvm::verifyModule(*module, &errs)) {
        outs() << errs.str() << "\n";
    }

    llvm::orc::ThreadSafeModule M(std::move(module), std::move(context));

    ExitOnErr(lljit->addIRModule(std::move(M)));

    // Look up the JIT'd function, cast it to a function pointer, then call it.
    auto Add1Addr = ExitOnErr(lljit->lookup("add1"));
    AddFunctionT add = Add1Addr.toPtr<int(int)>();
    return add;
}

int main(int argc, char* argv[]) {
    // Initialize LLVM.
    InitLLVM X(argc, argv);

    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();

    cl::ParseCommandLineOptions(argc, argv, "LLJITWithObjectCache");
    ExitOnErr.setBanner(std::string(argv[0]) + ": ");

    MyObjectCache MyCache;

    AddFunctionT add = runJITWithCache(MyCache);
    for (int i : {1, 2, 3, 4, 5, 6, 7, 8, 9}) {
        int r = add(i);
        outs() << "r=" << r << "\n";
    }
    return 0;
}
