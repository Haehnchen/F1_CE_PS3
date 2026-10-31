# Windows x86-64 build with llvm-mingw (clang + lld + libc++, UCRT).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(LLVM_MINGW "C:/Tools/llvm-mingw-20261006-ucrt-x86_64" CACHE PATH "llvm-mingw root")
set(CMAKE_C_COMPILER "${LLVM_MINGW}/bin/x86_64-w64-mingw32-clang.exe")
set(CMAKE_CXX_COMPILER "${LLVM_MINGW}/bin/x86_64-w64-mingw32-clang++.exe")
set(CMAKE_RC_COMPILER "${LLVM_MINGW}/bin/x86_64-w64-mingw32-windres.exe")
