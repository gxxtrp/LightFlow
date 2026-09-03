# clang-darwin.cmake
# Toolchain for macOS using Clang

set(CMAKE_SYSTEM_NAME Darwin)

find_program(CLANG_EXECUTABLE NAMES clang
    PATHS "/opt/homebrew/opt/llvm/bin" "/usr/local/opt/llvm/bin"
)
find_program(CLANGXX_EXECUTABLE NAMES clang++
    PATHS "/opt/homebrew/opt/llvm/bin" "/usr/local/opt/llvm/bin"
)

if(CLANGXX_EXECUTABLE)
    set(CMAKE_C_COMPILER "${CLANG_EXECUTABLE}" CACHE FILEPATH "C compiler" FORCE)
    set(CMAKE_CXX_COMPILER "${CLANGXX_EXECUTABLE}" CACHE FILEPATH "C++ compiler" FORCE)
else()
    set(CMAKE_C_COMPILER clang CACHE FILEPATH "C compiler" FORCE)
    set(CMAKE_CXX_COMPILER clang++ CACHE FILEPATH "C++ compiler" FORCE)
endif()
