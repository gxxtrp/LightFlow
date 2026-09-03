cmake_minimum_required(VERSION 3.25)

project(LightFlow
    VERSION 0.1.0
    DESCRIPTION "High-performance RHI-agnostic C++23 task graph library"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Add custom CMake module directory
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)

# Global Options
option(LF_BUILD_TESTS "Build LightFlow unit tests" ON)
option(LF_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(LF_ENABLE_TRACY "Enable Tracy profiler instrumentation" OFF)
option(LF_ENABLE_ASAN "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(LF_ENABLE_TSAN "Enable ThreadSanitizer" OFF)

# Sanitizer Configuration
if(LF_ENABLE_ASAN AND LF_ENABLE_TSAN)
    message(FATAL_ERROR "LF_ENABLE_ASAN and LF_ENABLE_TSAN are mutually exclusive.")
endif()

if(LF_ENABLE_ASAN)
    if(MSVC)
        add_compile_options(/fsanitize=address)
    else()
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

if(LF_ENABLE_TSAN)
    if(MSVC)
        message(WARNING "ThreadSanitizer is not supported on MSVC.")
    else()
        add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
        add_link_options(-fsanitize=thread)
    endif()
endif()

# Tracy Profiler Integration
if(LF_ENABLE_TRACY)
    find_package(Tracy QUIET)
    if(NOT Tracy_FOUND)
        include(FetchContent)
        FetchContent_Declare(
            tracy
            GIT_REPOSITORY https://github.com/wolfpld/tracy.git
            GIT_TAG        v0.11.1
            GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(tracy)
    endif()
endif()

# Library Target: LightFlow
add_library(LightFlow STATIC
    src/lightflow.cpp
)
add_library(lf::LightFlow ALIAS LightFlow)

target_include_directories(LightFlow
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(LightFlow PUBLIC cxx_std_23)
lightflow_set_compiler_warnings(LightFlow)

if(LF_ENABLE_TRACY)
    target_compile_definitions(LightFlow PUBLIC LF_ENABLE_TRACY=1 TRACY_ENABLE=1)
    target_link_libraries(LightFlow PUBLIC Tracy::TracyClient)
endif()

# Tests Subdirectory
if(LF_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
