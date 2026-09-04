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

// =============================================================================
// Custom MemoryCallbacks & Sized Deallocation Tests
// =============================================================================

namespace {

struct CustomTrackingAllocator {
    usize alloc_calls{0};
    usize free_calls{0};
    usize total_bytes_allocated{0};
    usize last_alloc_bytes{0};
    usize last_alloc_alignment{0};
    usize last_free_bytes{0};
    usize last_free_alignment{0};
    void* last_free_ptr{nullptr};
    bool simulate_oom{false};
};

void* test_custom_alloc(usize bytes, usize alignment, void* user_data) noexcept {
    auto* tracker = static_cast<CustomTrackingAllocator*>(user_data);
    if (tracker == nullptr) {
        return nullptr;
    }
    tracker->alloc_calls++;
    tracker->last_alloc_bytes = bytes;
    tracker->last_alloc_alignment = alignment;

    if (tracker->simulate_oom) {
        return nullptr;
    }

#if defined(_WIN32)
    void* ptr = _aligned_malloc(bytes, alignment);
#else
    void* ptr = nullptr;
    int res = ::posix_memalign(&ptr, alignment, bytes);
    if (res != 0) {
        return nullptr;
    }
#endif
    if (ptr != nullptr) {
        tracker->total_bytes_allocated += bytes;
    }
    return ptr;
}

void test_custom_free(void* ptr, usize bytes, usize alignment, void* user_data) noexcept {
    auto* tracker = static_cast<CustomTrackingAllocator*>(user_data);
    if (tracker != nullptr) {
        tracker->free_calls++;
        tracker->last_free_bytes = bytes;
        tracker->last_free_alignment = alignment;
        tracker->last_free_ptr = ptr;
    }
    if (ptr == nullptr) {
        return;
    }
#if defined(_WIN32)
    _aligned_free(ptr);
#else
    ::free(ptr);
#endif
}

} // anonymous namespace

TEST_CASE("MemoryCallbacks and platform_memory_callbacks validation", "[memory][callbacks]") {
    SECTION("platform_memory_callbacks validity") {
        MemoryCallbacks platform = platform_memory_callbacks();
        REQUIRE(platform.is_valid());
        REQUIRE(platform.alloc != nullptr);
        REQUIRE(platform.free != nullptr);
        REQUIRE(platform.user_data == nullptr);

        // Verify alloc and free work with 64KB alignment
        void* mem = platform.alloc(SLAB_SIZE, SLAB_SIZE, nullptr);
        REQUIRE(mem != nullptr);
        REQUIRE(reinterpret_cast<std::uintptr_t>(mem) % SLAB_SIZE == 0);
        platform.free(mem, SLAB_SIZE, SLAB_SIZE, nullptr);
    }

    SECTION("is_valid checks for partial or missing callbacks") {
        MemoryCallbacks empty{};
        REQUIRE_FALSE(empty.is_valid());

        MemoryCallbacks alloc_only{
            .alloc = &test_custom_alloc,
            .free = nullptr,
            .user_data = nullptr
        };
        REQUIRE_FALSE(alloc_only.is_valid());

        MemoryCallbacks free_only{
            .alloc = nullptr,
            .free = &test_custom_free,
            .user_data = nullptr
        };
        REQUIRE_FALSE(free_only.is_valid());

        MemoryCallbacks complete{
            .alloc = &test_custom_alloc,
            .free = &test_custom_free,
            .user_data = nullptr
        };
        REQUIRE(complete.is_valid());
    }
}

TEST_CASE("BlockPool custom MemoryCallbacks tracking and sized deallocation", "[memory][callbacks]") {
    CustomTrackingAllocator tracker{};
    MemoryCallbacks callbacks{
        .alloc = &test_custom_alloc,
        .free = &test_custom_free,
        .user_data = &tracker
    };

    constexpr usize INITIAL_SLABS = 4;
    constexpr usize CHUNK_SLABS = 4;

    {
        BlockPool pool(callbacks, INITIAL_SLABS, CHUNK_SLABS, true);

        REQUIRE(pool.callbacks().is_valid());
        REQUIRE(tracker.alloc_calls == 1);
        REQUIRE(tracker.last_alloc_bytes == INITIAL_SLABS * SLAB_SIZE);
        REQUIRE(tracker.last_alloc_alignment == SLAB_SIZE);
        REQUIRE(pool.available_slabs() == INITIAL_SLABS);
        REQUIRE(pool.total_slabs() == INITIAL_SLABS);
        REQUIRE(pool.chunk_count() == 1);

        std::vector<Slab*> slabs;
        slabs.reserve(INITIAL_SLABS);
        for (usize i = 0; i < INITIAL_SLABS; ++i) {
            Slab* slab = pool.acquire_slab();
            REQUIRE(slab != nullptr);
            REQUIRE(reinterpret_cast<std::uintptr_t>(slab) % CACHELINE_SIZE == 0);
            REQUIRE(reinterpret_cast<std::uintptr_t>(slab->payload()) % CACHELINE_SIZE == 0);
            std::memset(slab->payload(), static_cast<int>(0xAA + i), Slab::PAYLOAD_SIZE);
            slabs.push_back(slab);
        }
        REQUIRE(pool.available_slabs() == 0);

        // Dynamic growth: acquiring a 5th slab triggers allocate_chunk_and_acquire()
        Slab* extra_slab = pool.acquire_slab();
        REQUIRE(extra_slab != nullptr);
        REQUIRE(tracker.alloc_calls == 2);
        REQUIRE(tracker.last_alloc_bytes == CHUNK_SLABS * SLAB_SIZE);
        REQUIRE(tracker.last_alloc_alignment == SLAB_SIZE);
        REQUIRE(pool.chunk_count() == 2);
        REQUIRE(pool.total_slabs() == INITIAL_SLABS + CHUNK_SLABS);
        REQUIRE(pool.available_slabs() == CHUNK_SLABS - 1);

        // Release all slabs back to pool
        pool.release_slab(extra_slab);
        for (auto* slab : slabs) {
            pool.release_slab(slab);
        }
        REQUIRE(pool.available_slabs() == INITIAL_SLABS + CHUNK_SLABS);

        // Before pool destruction, no chunks freed yet
        REQUIRE(tracker.free_calls == 0);
    }

    // BlockPool has destructed: verify sized and aligned deallocation for all chunks
    REQUIRE(tracker.free_calls == 2);
    REQUIRE(tracker.last_free_bytes == CHUNK_SLABS * SLAB_SIZE);
    REQUIRE(tracker.last_free_alignment == SLAB_SIZE);
    REQUIRE(tracker.last_free_ptr != nullptr);
}

TEST_CASE("BlockPool strict fail-fast on simulated out-of-memory", "[memory][callbacks][fail_fast]") {
    CustomTrackingAllocator tracker{};
    MemoryCallbacks callbacks{
        .alloc = &test_custom_alloc,
        .free = &test_custom_free,
        .user_data = &tracker
    };

    SECTION("Initial allocation OOM fails fast without fallback to libc heap") {
        tracker.simulate_oom = true;

        BlockPool pool(callbacks, 4, 4, true);
        REQUIRE(pool.total_slabs() == 0);
        REQUIRE(pool.available_slabs() == 0);
        REQUIRE(pool.chunk_count() == 0);
        REQUIRE(tracker.alloc_calls == 1);

        // acquire_slab should attempt growth, fail fast, and return nullptr
        Slab* slab = pool.acquire_slab();
        REQUIRE(slab == nullptr);
        REQUIRE(tracker.alloc_calls == 2); // 1 init attempt + 1 growth attempt
        REQUIRE(tracker.free_calls == 0);
    }

    SECTION("Dynamic growth OOM fails fast without corrupting existing slabs") {
        tracker.simulate_oom = false;

        constexpr usize INITIAL_SLABS = 2;
        BlockPool pool(callbacks, INITIAL_SLABS, 2, true);
        REQUIRE(pool.total_slabs() == INITIAL_SLABS);
        REQUIRE(tracker.alloc_calls == 1);

        Slab* s1 = pool.acquire_slab();
        Slab* s2 = pool.acquire_slab();
        REQUIRE(s1 != nullptr);
        REQUIRE(s2 != nullptr);
        REQUIRE(pool.available_slabs() == 0);

        // Simulate OOM during dynamic growth
        tracker.simulate_oom = true;
        Slab* s3 = pool.acquire_slab();
        REQUIRE(s3 == nullptr); // Failed fast, returned nullptr
        REQUIRE(tracker.alloc_calls == 2);

        // Releasing existing slabs back to pool works safely
        pool.release_slab(s1);
        REQUIRE(pool.available_slabs() == 1);

        // Can re-acquire from freelist without any new allocation call
        Slab* reacquired = pool.acquire_slab();
        REQUIRE(reacquired == s1);
        REQUIRE(tracker.alloc_calls == 2); // No new alloc call

        pool.release_slab(reacquired);
        pool.release_slab(s2);
        REQUIRE(pool.available_slabs() == 2);
    }
}

TEST_CASE("SlabArena integration with custom MemoryCallbacks BlockPool", "[memory][callbacks][arena]") {
    CustomTrackingAllocator tracker{};
    MemoryCallbacks callbacks{
        .alloc = &test_custom_alloc,
        .free = &test_custom_free,
        .user_data = &tracker
    };

    BlockPool pool(callbacks, 2, 2, true);
    SlabArena arena(pool);

    // Bump allocate within first slab
    void* p1 = arena.allocate(1024, 64);
    REQUIRE(p1 != nullptr);
    REQUIRE(reinterpret_cast<std::uintptr_t>(p1) % 64 == 0);

    // Fill up the first slab to force acquiring a second slab from the custom BlockPool
    constexpr usize ALLOC_SIZE = 16384;
    for (usize i = 0; i < 5; ++i) {
        void* p = arena.allocate(ALLOC_SIZE, 64);
        REQUIRE(p != nullptr);
        REQUIRE(reinterpret_cast<std::uintptr_t>(p) % 64 == 0);
    }

    // Slabs have been chained in SlabArena
    arena.reset();

    // After reset, all slabs are returned to BlockPool freelist
    REQUIRE(pool.available_slabs() == pool.total_slabs());
}

TEST_CASE("BlockPool default constructor backwards compatibility", "[memory][callbacks][compat]") {
    BlockPool pool;
    REQUIRE(pool.callbacks().is_valid());
    REQUIRE(pool.total_slabs() == BlockPool::DEFAULT_INITIAL_SLABS);
    REQUIRE(pool.available_slabs() == BlockPool::DEFAULT_INITIAL_SLABS);

    Slab* slab = pool.acquire_slab();
    REQUIRE(slab != nullptr);
    REQUIRE(pool.available_slabs() == BlockPool::DEFAULT_INITIAL_SLABS - 1);

    pool.release_slab(slab);
    REQUIRE(pool.available_slabs() == BlockPool::DEFAULT_INITIAL_SLABS);
}
