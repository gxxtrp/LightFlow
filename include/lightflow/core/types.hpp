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

// Library version string
LF_NODISCARD std::string_view version() noexcept;

} // namespace lf
