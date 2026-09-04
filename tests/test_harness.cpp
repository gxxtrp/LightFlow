#include "test_harness.hpp"

#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <cstdlib>

#if defined(_WIN32)
    #include <malloc.h>
#else
    #include <stdlib.h>
#endif

namespace lf::test {

namespace {
TestHarnessStats s_stats{};
}

TestHarnessStats& TestHarnessAllocator::stats() noexcept {
    return s_stats;
}

void TestHarnessAllocator::reset_stats() noexcept {
    s_stats.alloc_count.store(0, std::memory_order_relaxed);
    s_stats.free_count.store(0, std::memory_order_relaxed);
    s_stats.allocated_bytes.store(0, std::memory_order_relaxed);
    s_stats.freed_bytes.store(0, std::memory_order_relaxed);
    s_stats.current_bytes.store(0, std::memory_order_relaxed);
    s_stats.peak_bytes.store(0, std::memory_order_relaxed);
}

void* TestHarnessAllocator::alloc(usize bytes, usize alignment, void* /*user_data*/) noexcept {
    void* ptr = nullptr;
#if defined(_WIN32)
    ptr = _aligned_malloc(bytes, alignment);
#else
    int res = ::posix_memalign(&ptr, alignment, bytes);
    if (res != 0) {
        return nullptr;
    }
#endif
    if (ptr != nullptr) {
        s_stats.alloc_count.fetch_add(1, std::memory_order_relaxed);
        s_stats.allocated_bytes.fetch_add(bytes, std::memory_order_relaxed);
        usize current = s_stats.current_bytes.fetch_add(bytes, std::memory_order_relaxed) + bytes;
        usize prev_peak = s_stats.peak_bytes.load(std::memory_order_relaxed);
        while (current > prev_peak && !s_stats.peak_bytes.compare_exchange_weak(prev_peak, current, std::memory_order_relaxed)) {}
    }
    return ptr;
}

void TestHarnessAllocator::free(void* ptr, usize bytes, usize /*alignment*/, void* /*user_data*/) noexcept {
    if (ptr == nullptr) {
        return;
    }
    s_stats.free_count.fetch_add(1, std::memory_order_relaxed);
    s_stats.freed_bytes.fetch_add(bytes, std::memory_order_relaxed);
    s_stats.current_bytes.fetch_sub(bytes, std::memory_order_relaxed);
#if defined(_WIN32)
    _aligned_free(ptr);
#else
    ::free(ptr);
#endif
}

MemoryCallbacks TestHarnessAllocator::callbacks() noexcept {
    return MemoryCallbacks{
        .alloc = &TestHarnessAllocator::alloc,
        .free = &TestHarnessAllocator::free,
        .user_data = nullptr
    };
}

void install_test_allocator() noexcept {
    if (!BlockPool::is_global_initialized()) {
        BlockPool::set_global_callbacks(TestHarnessAllocator::callbacks());
    }
}

namespace {
struct TestHarnessStaticBootstrap {
    TestHarnessStaticBootstrap() {
        install_test_allocator();
    }
};
static TestHarnessStaticBootstrap s_staticBootstrap;
} // anonymous namespace

class TestHarnessEventListener : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(Catch::TestRunInfo const& /*testRunInfo*/) override {
        install_test_allocator();
    }
};

} // namespace lf::test

CATCH_REGISTER_LISTENER(lf::test::TestHarnessEventListener)
