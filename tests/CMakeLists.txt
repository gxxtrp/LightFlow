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

add_executable(lf_smoke_test
    test_smoke.cpp
)

target_link_libraries(lf_smoke_test
    PRIVATE
        lf::LightFlow
        Catch2::Catch2WithMain
)

target_compile_features(lf_smoke_test PRIVATE cxx_std_23)
lightflow_set_compiler_warnings(lf_smoke_test)

catch_discover_tests(lf_smoke_test)
