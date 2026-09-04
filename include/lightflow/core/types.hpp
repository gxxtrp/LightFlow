#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace lf {

// Semantic version constants
inline constexpr std::uint32_t VERSION_MAJOR = 0;
inline constexpr std::uint32_t VERSION_MINOR = 1;
inline constexpr std::uint32_t VERSION_PATCH = 0;

// Standard cache line size for task node alignment (64 bytes)
inline constexpr std::size_t CACHELINE_SIZE = 64;

// Standard memory block slab size (64 KB)
inline constexpr std::size_t SLAB_SIZE = 64 * 1024;

// Portable compiler attributes
#if defined(_MSC_VER)
    #define LF_FORCE_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
    #define LF_FORCE_INLINE inline __attribute__((always_inline))
#else
    #define LF_FORCE_INLINE inline
#endif

#define LF_NODISCARD [[nodiscard]]

// Cacheline alignment macro preventing false sharing
#define LF_ALIGN_CACHELINE alignas(lf::CACHELINE_SIZE)

// Assertion macro compiling to zero instructions in release builds
#if !defined(NDEBUG)
    #include <cassert>
    #define LF_ASSERT(expr) assert(expr)
#else
    #define LF_ASSERT(expr) ((void)0)
#endif

// Portable CPU pause / yield instruction for spin loops
LF_FORCE_INLINE void cpu_pause() noexcept {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    #if defined(_MSC_VER)
        _mm_pause();
    #else
        __builtin_ia32_pause();
    #endif
#elif defined(__aarch64__) || defined(_M_ARM64)
    #if defined(__GNUC__) || defined(__clang__)
        asm volatile("yield" ::: "memory");
    #else
        std::this_thread::yield();
    #endif
#else
    std::this_thread::yield();
#endif
}

// Fixed-width integer typedefs
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

using usize = std::size_t;

// Sentinel value indicating a non-worker thread
inline constexpr u32 INVALID_WORKER_INDEX = static_cast<u32>(-1);

// Library version string
LF_NODISCARD std::string_view version() noexcept;

// Zero-RTTI and zero-exception discipline query functions
LF_NODISCARD bool is_rtti_enabled() noexcept;
LF_NODISCARD bool are_exceptions_enabled() noexcept;

} // namespace lf
