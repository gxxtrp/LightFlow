#pragma once

#include <lightflow/core/memory_pool.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace lf::test {

/// Telemetry metrics tracking allocations routed through TestHarnessAllocator.
struct TestHarnessStats {
    std::atomic<usize> alloc_count{0};
    std::atomic<usize> free_count{0};
    std::atomic<usize> allocated_bytes{0};
    std::atomic<usize> freed_bytes{0};
    std::atomic<usize> current_bytes{0};
    std::atomic<usize> peak_bytes{0};
};

/// Host tracking allocator providing MemoryCallbacks for testing.
/// Simulates game engine / console host virtual memory system.
class TestHarnessAllocator {
public:
    static void* alloc(usize bytes, usize alignment, void* user_data) noexcept;
    static void free(void* ptr, usize bytes, usize alignment, void* user_data) noexcept;
    static MemoryCallbacks callbacks() noexcept;
    static TestHarnessStats& stats() noexcept;
    static void reset_stats() noexcept;
};

/// Installs TestHarnessAllocator as the global BlockPool bootstrap allocator if not already initialized.
void install_test_allocator() noexcept;

} // namespace lf::test
