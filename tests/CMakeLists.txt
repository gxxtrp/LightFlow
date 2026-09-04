# tests/CMakeLists.txt

find_package(Catch2 3 QUIET)
if(NOT Catch2_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.8.1
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(Catch2)
endif()

include(Catch)

function(lightflow_add_test target_name source_file)
    add_executable(${target_name} ${source_file} test_harness.cpp)
    target_link_libraries(${target_name}
        PRIVATE
            lf::LightFlow
            Catch2::Catch2WithMain
    )
    target_compile_features(${target_name} PRIVATE cxx_std_23)
    lightflow_set_compiler_warnings(${target_name})
    catch_discover_tests(${target_name})
endfunction()

# ==============================================================================
# Consolidated Production Test Suites
# ==============================================================================

option(LF_ENABLE_TASKFLOW_BENCHMARK "Enable Taskflow comparative benchmarks" ON)
if(LF_ENABLE_TASKFLOW_BENCHMARK)
    set(TF_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(TF_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(TF_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
    include(FetchContent)
    FetchContent_Declare(
        Taskflow
        GIT_REPOSITORY https://github.com/taskflow/taskflow.git
        GIT_TAG        v3.8.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(Taskflow)
endif()

lightflow_add_test(lf_task_graph_test          test_task_graph.cpp)
lightflow_add_test(lf_scheduler_test           test_scheduler.cpp)
lightflow_add_test(lf_memory_test              test_memory.cpp)
lightflow_add_test(lf_gpu_sync_test            test_gpu_sync.cpp)
lightflow_add_test(lf_concurrency_stress_test  test_concurrency_stress.cpp)
lightflow_add_test(lf_comparison_test          test_comparison.cpp)

target_include_directories(lf_comparison_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(LF_ENABLE_TASKFLOW_BENCHMARK AND TARGET Taskflow)
    target_link_libraries(lf_comparison_test PRIVATE Taskflow)
    target_compile_definitions(lf_comparison_test PRIVATE LF_HAS_TASKFLOW=1)
    if(MSVC)
        target_compile_options(lf_comparison_test PRIVATE /EHsc)
    else()
        target_compile_options(lf_comparison_test PRIVATE -fexceptions)
    endif()
    get_target_property(TF_INC Taskflow INTERFACE_INCLUDE_DIRECTORIES)
    if(TF_INC)
        target_include_directories(lf_comparison_test SYSTEM PRIVATE ${TF_INC})
    endif()
endif()

if(APPLE)
    target_compile_definitions(lf_comparison_test PRIVATE _XOPEN_SOURCE=600)
endif()

if(TARGET Tracy::TracyClient)
    lightflow_add_test(lf_tracy_test           test_tracy.cpp)
endif()

