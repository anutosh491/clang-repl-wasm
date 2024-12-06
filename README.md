Exact set of instructions to get clang-repl running in browser.

# Installations & Dependencies
1) Install micromamba on your system through the following [recommended way](https://mamba.readthedocs.io/en/latest/installation/micromamba-installation.html#automatic-install)
2) We now possibly need `cmake` and `emsdk`.
```
// for cmake
micromamba create -n wasm-build cmake
micromamba activate wasm-build

// for emsdk we have been using 3.1.45 but later versions 
// like 3.1.58 can be experimented with
cd $HOME
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk
./emsdk install 3.1.45
./emsdk activate 3.1.45
source $HOME/emsdk/emsdk_env.sh

```
3) Now fetch [llvm](https://github.com/emscripten-forge/recipes/blob/main/recipes/recipes_emscripten/llvm/recipe.yaml) from [emscripten-forge](https://github.com/emscripten-forge/recipes) in a new environment named wasm-host. Any version `>= 19.1.5` should do.
Emscripten-forge is basically a repo for hosting packages built for webassembly (for targets like `emscripten-wasm32`). Hence we have `llvm` (`clang` && `lld`) built for wasm against emscripten and hosted there.
To achieve this use the following
```
micromamba create -n wasm-host
micromamba activate wasm-host
micromamba install llvm -c https://repo.mamba.pm/emscripten-forge
// come back to wasm-build now
micromamba deactivate
```

# Building & Running the project

1) We shall now move towards building our project. A project structure like the following would make sense.
```
clang-repl-wasm/
├── CMakeLists.txt              # Primary CMake configuration file
├── CompilerModule.cpp          # Implementation of the REPL module logic
├── build/                      # Directory for build outputs
│   └── index.html              # Generated build files and binaries
```
The `CMakeLists.txt`, `CompilerModule.cpp` and `index.html` are present as source files above. 

2) Now move to the `build` folder which should be have a single `index.html` file and run the cmake configuration. We would be using emscripten's sysroot which contains standard header file and libraries that we would be using.
```
cd build

// export wasm-host as the PREFIX
// we still need to use wasm-build for cmake
export PREFIX=$MAMBA_ROOT_PREFIX/envs/wasm-host

// sysroot is usually present $HOME/emsdk/upstream/emscripten/cache/sysroot
export SYSROOT_PATH=/path/to/sysroot

emcmake cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH=$PREFIX \
    -DLLVM_DIR=$PREFIX/lib/cmake/llvm \
    -DLLD_DIR=$PREFIX/lib/cmake/lld \
    -DClang_DIR=$PREFIX/lib/cmake/clang \
    -DSYSROOT_PATH=$SYSROOT_PATH \
    ../
```

3) We are now interested in install the `Compiler.js`, `Compiler.wasm` and `Compiler.data` binaries. Done using 
```
emmake make
```

4) We should now be fully equipped to run clang-repl in browser. Attempt running it on local host through 
```
python3 -m http.server 8080
```

And you should see the following 


https://github.com/user-attachments/assets/1852aaa1-b1c5-4ea3-8ac2-22faf3774c26
