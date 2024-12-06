cmake_minimum_required(VERSION 3.20.0)
project(clang-wasm-repl)

find_package(LLVM REQUIRED CONFIG)
find_package(LLD REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)

message(STATUS "Using ClangConfig.cmake in: ${Clang_DIR}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")
message(STATUS "Using LLDConfig.cmake in: ${LLD_DIR}")
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")

set (CMAKE_CXX_STANDARD 17)
add_compile_options(-Wall -pedantic -fPIC)

include_directories(${LLVM_INCLUDE_DIRS})
include_directories(${CLANG_INCLUDE_DIRS})

add_executable(Compiler CompilerModule.cpp)

# Link against LLVM libraries
target_link_libraries(Compiler embind)
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
    # add sysroot location here
    --preload-file ${SYSROOT_PATH}/@/
)
