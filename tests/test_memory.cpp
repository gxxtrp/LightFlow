#include <catch2/catch_test_macros.hpp>
#include <lightflow/lightflow.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>
#include <vector>

#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__) || \
    (defined(__has_feature) && (__has_feature(thread_sanitizer) || __has_feature(address_sanitizer))) || \
    defined(LF_ENABLE_ASAN) || defined(LF_ENABLE_TSAN) || defined(LF_ENABLE_TRACY)
    #define LF_INSTRUMENTED_BUILD 1
#endif

// =============================================================================
// Global Allocation Tracker for Zero-Malloc Invariant Verification
// =============================================================================

namespace lf::test {

struct AllocationTracker {
    static inline std::atomic<bool> s_active{false};
    static inline std::atomic<size_t> s_allocationCount{0};
    static inline std::atomic<size_t> s_deallocationCount{0};
    static inline std::atomic<size_t> s_allocatedBytes{0};

    static void reset() noexcept {
        s_allocationCount.store(0, std::memory_order_relaxed);
        s_deallocationCount.store(0, std::memory_order_relaxed);
        s_allocatedBytes.store(0, std::memory_order_relaxed);
    }

    static void enable() noexcept {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        s_active.store(true, std::memory_order_seq_cst);
    }

    static void disable() noexcept {
        s_active.store(false, std::memory_order_seq_cst);
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }

    static size_t allocations() noexcept {
        return s_allocationCount.load(std::memory_order_relaxed);
    }

    static size_t bytes() noexcept {
        return s_allocatedBytes.load(std::memory_order_relaxed);
    }
};

} // namespace lf::test

void* operator new(std::size_t size) {
    if (lf::test::AllocationTracker::s_active.load(std::memory_order_relaxed)) {
        lf::test::AllocationTracker::s_allocationCount.fetch_add(1, std::memory_order_relaxed);
        lf::test::AllocationTracker::s_allocatedBytes.fetch_add(size, std::memory_order_relaxed);
    }
    void* p = std::malloc(size);
    if (p == nullptr) {
        std::abort();
    }
    return p;
}

void* operator new[](std::size_t size) {
    if (lf::test::AllocationTracker::s_active.load(std::memory_order_relaxed)) {
        lf::test::AllocationTracker::s_allocationCount.fetch_add(1, std::memory_order_relaxed);
        lf::test::AllocationTracker::s_allocatedBytes.fetch_add(size, std::memory_order_relaxed);
    }
    void* p = std::malloc(size);
    if (p == nullptr) {
        std::abort();
    }
    return p;
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    if (lf::test::AllocationTracker::s_active.load(std::memory_order_relaxed)) {
        lf::test::AllocationTracker::s_allocationCount.fetch_add(1, std::memory_order_relaxed);
        lf::test::AllocationTracker::s_allocatedBytes.fetch_add(size, std::memory_order_relaxed);
    }
    return std::malloc(size);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    if (lf::test::AllocationTracker::s_active.load(std::memory_order_relaxed)) {
        lf::test::AllocationTracker::s_allocationCount.fetch_add(1, std::memory_order_relaxed);
        lf::test::AllocationTracker::s_allocatedBytes.fetch_add(size, std::memory_order_relaxed);
    }
    return std::malloc(size);
}

void operator delete(void* p) noexcept {
    if (lf::test::AllocationTracker::s_active.load(std::memory_order_relaxed)) {
        lf::test::AllocationTracker::s_deallocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}

void operator delete[](void* p) noexcept {
    if (lf::test::AllocationTracker::s_active.load(std::memory_order_relaxed)) {
        lf::test::AllocationTracker::s_deallocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}

void operator delete(void* p, std::size_t) noexcept {
    if (lf::test::AllocationTracker::s_active.load(std::memory_order_relaxed)) {
        lf::test::AllocationTracker::s_deallocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}

void operator delete[](void* p, std::size_t) noexcept {
    if (lf::test::AllocationTracker::s_active.load(std::memory_order_relaxed)) {
        lf::test::AllocationTracker::s_deallocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}

void operator delete(void* p, const std::nothrow_t&) noexcept {
    if (lf::test::AllocationTracker::s_active.load(std::memory_order_relaxed)) {
        lf::test::AllocationTracker::s_deallocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}

void operator delete[](void* p, const std::nothrow_t&) noexcept {
    if (lf::test::AllocationTracker::s_active.load(std::memory_order_relaxed)) {
        lf::test::AllocationTracker::s_deallocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}

void operator delete(void* p, std::size_t, const std::nothrow_t&) noexcept {
    if (lf::test::AllocationTracker::s_active.load(std::memory_order_relaxed)) {
        lf::test::AllocationTracker::s_deallocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}

void operator delete[](void* p, std::size_t, const std::nothrow_t&) noexcept {
    if (lf::test::AllocationTracker::s_active.load(std::memory_order_relaxed)) {
        lf::test::AllocationTracker::s_deallocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}

using namespace lf;

// =============================================================================
// Slab and BlockPool Memory Invariants
// =============================================================================

TEST_CASE("Slab sizing and BlockPool acquire/release lifecycle", "[memory][slab]") {
    STATIC_REQUIRE(sizeof(Slab) == CACHELINE_SIZE);
    STATIC_REQUIRE(alignof(Slab) == CACHELINE_SIZE);
    STATIC_REQUIRE(Slab::HEADER_SIZE == 64);
    STATIC_REQUIRE(Slab::PAYLOAD_SIZE == 65472);
    STATIC_REQUIRE(Slab::HEADER_SIZE + Slab::PAYLOAD_SIZE == SLAB_SIZE);

    constexpr usize INITIAL_SLABS = 8;
    BlockPool pool(INITIAL_SLABS, INITIAL_SLABS, true);

    REQUIRE(pool.total_slabs() == INITIAL_SLABS);
    REQUIRE(pool.available_slabs() == INITIAL_SLABS);

    std::vector<Slab*> slabs;
    slabs.reserve(INITIAL_SLABS);

    for (usize i = 0; i < INITIAL_SLABS; ++i) {
        Slab* slab = pool.acquire_slab();
        REQUIRE(slab != nullptr);
        REQUIRE(reinterpret_cast<std::uintptr_t>(slab) % CACHELINE_SIZE == 0);
        REQUIRE(reinterpret_cast<std::uintptr_t>(slab->payload()) % CACHELINE_SIZE == 0);

        std::memset(slab->payload(), static_cast<int>(i & 0xFF), Slab::PAYLOAD_SIZE);
        slabs.push_back(slab);
    }

    REQUIRE(pool.available_slabs() == 0);

    for (auto* slab : slabs) {
        pool.release_slab(slab);
    }
    REQUIRE(pool.available_slabs() == INITIAL_SLABS);
}

// =============================================================================
// SlabArena Bump Allocation
// =============================================================================

TEST_CASE("SlabArena monotonic bump allocation and alignment", "[memory][arena]") {
    BlockPool pool(8, 8, true);
    SlabArena arena(pool);

    // 64-byte alignment
    void* p1 = arena.allocate(128, 64);
    REQUIRE(p1 != nullptr);
    REQUIRE(reinterpret_cast<std::uintptr_t>(p1) % 64 == 0);

    void* p2 = arena.allocate(64, 64);
    REQUIRE(p2 != nullptr);
    REQUIRE(reinterpret_cast<std::uintptr_t>(p2) % 64 == 0);
    REQUIRE(static_cast<std::byte*>(p2) >= static_cast<std::byte*>(p1) + 128);

    // Reset restores available space
    arena.reset();
    void* p3 = arena.allocate(128, 64);
    REQUIRE(p3 != nullptr);
    REQUIRE(p3 == p1); // Monotonic bump pointer reset to beginning
}

// =============================================================================
// MoveOnlyTask Small Buffer Optimization (SBO)
// =============================================================================

TEST_CASE("MoveOnlyTask 48-byte SBO inline execution and lifecycle", "[memory][task]") {
    SECTION("Fits in 48-byte SBO: zero dynamic allocation") {
        static std::atomic<i32> destructCount{0};
        destructCount.store(0, std::memory_order_relaxed);

        struct SmallCallable {
            u64 v1{10};
            u64 v2{20};
            void operator()() const noexcept {
                CHECK(v1 == 10);
                CHECK(v2 == 20);
            }
            ~SmallCallable() noexcept {
                destructCount.fetch_add(1, std::memory_order_relaxed);
            }
        };

        static_assert(sizeof(SmallCallable) <= MoveOnlyTask::SBO_SIZE);

        {
            MoveOnlyTask task(SmallCallable{});
            REQUIRE(task.valid());
            task();
        }
        CHECK(destructCount.load(std::memory_order_relaxed) >= 2);
    }

    SECTION("MoveOnlyTask move assignment and execution") {
        std::atomic<bool> done{false};
        MoveOnlyTask t1([&done]() noexcept {
            done.store(true, std::memory_order_release);
        });

        MoveOnlyTask t2 = std::move(t1);
        REQUIRE_FALSE(t1.valid());
        REQUIRE(t2.valid());

        t2();
        CHECK(done.load(std::memory_order_acquire));
    }
}

// =============================================================================
// Steady-State Zero-Allocation Guarantee
// =============================================================================

TEST_CASE("Steady-state frame loop: Zero heap allocations across 100 frames", "[memory][zero_malloc]") {
    SchedulerConfig config{.workerCount = 4, .initialDequeCapacity = 2048};
    TaskScheduler scheduler(config);

    TaskGraph graph;
    constexpr usize TASK_COUNT = 1000;
    std::atomic<usize> counter{0};

    auto root = graph.emplace([&counter]() noexcept {
        counter.fetch_add(1, std::memory_order_relaxed);
    });

    auto barrier = graph.emplace([&counter]() noexcept {
        counter.fetch_add(1, std::memory_order_relaxed);
    });

    for (usize i = 0; i < TASK_COUNT - 2; ++i) {
        auto child = graph.emplace([&counter]() noexcept {
            counter.fetch_add(1, std::memory_order_relaxed);
        });
        root.precede(child);
        child.precede(barrier);
    }

    // Warm-up
    for (int w = 0; w < 3; ++w) {
        scheduler.runAndWait(graph);
    }
    REQUIRE(counter.load() == TASK_COUNT * 3);

    // Track steady-state frame loop allocations
    lf::test::AllocationTracker::reset();
    lf::test::AllocationTracker::enable();

    constexpr usize NUM_FRAMES = 100;
    for (usize f = 0; f < NUM_FRAMES; ++f) {
        scheduler.runAndWait(graph);
    }

    lf::test::AllocationTracker::disable();

    REQUIRE(counter.load() == TASK_COUNT * (NUM_FRAMES + 3));

#if !defined(LF_INSTRUMENTED_BUILD)
    // In release mode without sanitizers, MUST have strictly 0 heap allocations
    CHECK(lf::test::AllocationTracker::allocations() == 0);
    CHECK(lf::test::AllocationTracker::bytes() == 0);
#endif
}
