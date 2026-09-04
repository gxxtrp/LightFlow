#include <lightflow/lightflow.hpp>

// Compile-time verification that RTTI and exceptions are strictly disabled
#if defined(__clang__) || defined(__GNUC__)
    #if defined(__GXX_RTTI)
        #error "LightFlow must be compiled without RTTI (-fno-rtti)"
    #endif
    #if defined(__EXCEPTIONS)
        #error "LightFlow must be compiled without exceptions (-fno-exceptions)"
    #endif
#elif defined(_MSC_VER)
    #if defined(_CPPRTTI)
        #error "LightFlow must be compiled without RTTI (/GR-)"
    #endif
    #if defined(_CPPUNWIND)
        #error "LightFlow must be compiled without exceptions (/EHs-c-)"
    #endif
#endif

namespace lf {

std::string_view version() noexcept {
    return "0.1.0";
}

bool is_rtti_enabled() noexcept {
#if defined(__GXX_RTTI) || defined(_CPPRTTI)
    return true;
#else
    return false;
#endif
}

bool are_exceptions_enabled() noexcept {
#if defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    return true;
#else
    return false;
#endif
}

} // namespace lf
