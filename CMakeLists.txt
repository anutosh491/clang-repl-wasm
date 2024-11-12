cmake_minimum_required(VERSION 3.20.0)
project(ClangWASMREPL)

find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)
find_package(LLD REQUIRED CONFIG)

message(STATUS "Using ClangConfig.cmake in: ${Clang_DIR}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")

set (CMAKE_CXX_STANDARD 17)
add_compile_options(-Wall -pedantic -fPIC)

include_directories(include)

include_directories(${LLVM_INCLUDE_DIRS})
include_directories(${CLANG_INCLUDE_DIRS})
include_directories(${LLD_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_definitions(${LLVM_DEFINITIONS_LIST})

add_executable(Compiler CompilerModule.cpp)

llvm_map_components_to_libnames(llvm_libs Core MC Support TargetParser WebAssembly)

# Link against LLVM libraries
target_link_libraries(Compiler embind)
target_link_libraries(Compiler lldWasm)
target_link_libraries(Compiler clangInterpreter)

target_link_options(Compiler PRIVATE
    -O1
    -sMODULARIZE
    -sEXPORT_ES6=1
    -sASSERTIONS
    -sALLOW_MEMORY_GROWTH=1
    -sINITIAL_MEMORY=128MB
    -sTOTAL_STACK=32MB
    -sMAIN_MODULE=1
    -sUSE_SDL=2
    -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,stringToNewUTF8,getValue,setValue
    -sEXPORTED_FUNCTIONS=_malloc,_free,__ZTIN10emscripten3valE
    --preload-file ${EMSCRIPTEN_SYSROOT}/@/
    #--preload-file /media/hdstorage/builds/llvm-wasm-ems/lib/clang/19/include@/lib/clang/19/include
    #--preload-file /opt/emscripten-llvm/lib@/lib
)
