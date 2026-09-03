# clang-windows.cmake
# Toolchain for Windows using clang-cl

set(CMAKE_SYSTEM_NAME Windows)

find_program(CLANG_CL_EXECUTABLE NAMES clang-cl.exe clang-cl
    PATHS
    "C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/VC/Tools/Llvm/x64/bin"
    "C:/Program Files/Microsoft Visual Studio/18/BuildTools/VC/Tools/Llvm/x64/bin"
    "C:/Program Files/LLVM/bin"
)

find_program(RC_EXECUTABLE NAMES rc.exe llvm-rc.exe
    PATHS
    "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64"
    "C:/Program Files (x86)/Windows Kits/10/bin/10.0.22621.0/x64"
    "C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/VC/Tools/Llvm/x64/bin"
)

find_program(MT_EXECUTABLE NAMES mt.exe llvm-mt.exe
    PATHS
    "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64"
    "C:/Program Files (x86)/Windows Kits/10/bin/10.0.22621.0/x64"
    "C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/VC/Tools/Llvm/x64/bin"
)

if(CLANG_CL_EXECUTABLE)
    set(CMAKE_C_COMPILER "${CLANG_CL_EXECUTABLE}" CACHE FILEPATH "C compiler" FORCE)
    set(CMAKE_CXX_COMPILER "${CLANG_CL_EXECUTABLE}" CACHE FILEPATH "C++ compiler" FORCE)
else()
    set(CMAKE_C_COMPILER clang-cl CACHE FILEPATH "C compiler" FORCE)
    set(CMAKE_CXX_COMPILER clang-cl CACHE FILEPATH "C++ compiler" FORCE)
endif()

if(RC_EXECUTABLE)
    set(CMAKE_RC_COMPILER "${RC_EXECUTABLE}" CACHE FILEPATH "Resource compiler" FORCE)
endif()

if(MT_EXECUTABLE)
    set(CMAKE_MT "${MT_EXECUTABLE}" CACHE FILEPATH "Manifest tool" FORCE)
endif()
