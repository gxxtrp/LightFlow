# clang-linux.cmake
# Toolchain for Linux using Clang

set(CMAKE_SYSTEM_NAME Linux)

set(CMAKE_C_COMPILER clang CACHE FILEPATH "C compiler" FORCE)
set(CMAKE_CXX_COMPILER clang++ CACHE FILEPATH "C++ compiler" FORCE)
