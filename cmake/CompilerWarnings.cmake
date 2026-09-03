# CompilerWarnings.cmake
# Strict compiler warnings and warnings-as-errors configuration for LightFlow

function(lightflow_set_compiler_warnings target_name)
    if(NOT TARGET ${target_name})
        message(FATAL_ERROR "lightflow_set_compiler_warnings: Target ${target_name} does not exist.")
    endif()

    set(CLANG_WARNINGS
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Wcast-align
        -Wunused
        -Woverloaded-virtual
        -Wnull-dereference
        -Wdouble-promotion
        -Wformat=2
        -Wimplicit-fallthrough
    )

    set(CLANG_CL_WARNINGS
        /W4
        /permissive-
        /w14242 # conversion from 'type1' to 'type2', possible loss of data
        /w14254 # conversion from 'type1' to 'type2', possible loss of data
        /w14263 # member function does not override any base class virtual member function
        /w14265 # class has virtual functions, but destructor is not virtual
        /w14287 # unsigned/negative constant mismatch
        /w14296 # expression is always 'boolean_value'
        /w14311 # pointer truncation
        /w14545 # expression before comma evaluates to a function which is missing an argument list
        /w14546 # function call before comma missing argument list
        /w14547 # operator before comma has no effect
        /w14549 # operator before comma has no effect
        /w14555 # expression has no effect
        /w14619 # pragma warning: there is no warning number 'number'
        /w14640 # enable warning on thread un-safe static member initialization
        /w14826 # conversion is sign-extended
        /w14905 # wide string literal cast to 'LPSTR'
        /w14906 # string literal cast to 'LPWSTR'
        /w14928 # illegal copy-initialization
    )

    set(MSVC_WARNINGS
        /W4
        /permissive-
    )

    set(GCC_WARNINGS
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Wcast-align
        -Wunused
        -Woverloaded-virtual
        -Wnull-dereference
        -Wdouble-promotion
        -Wformat=2
        -Wimplicit-fallthrough
    )

    if(LF_WARNINGS_AS_ERRORS)
        list(APPEND CLANG_WARNINGS -Werror)
        list(APPEND GCC_WARNINGS -Werror)
        list(APPEND CLANG_CL_WARNINGS /WX)
        list(APPEND MSVC_WARNINGS /WX)
    endif()

    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        if(MSVC)
            target_compile_options(${target_name} PRIVATE ${CLANG_CL_WARNINGS})
        else()
            target_compile_options(${target_name} PRIVATE ${CLANG_WARNINGS})
        endif()
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(${target_name} PRIVATE ${GCC_WARNINGS})
    elseif(MSVC)
        target_compile_options(${target_name} PRIVATE ${MSVC_WARNINGS})
    endif()
endfunction()
