#include <catch2/catch_test_macros.hpp>
#include <lightflow/lightflow.hpp>
#include "test_harness.hpp"

#include <array>
#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>
#include <vector>

#if !defined(_WIN32)
    #include <sys/mman.h>
    #include <sys/types.h>
    #include <sys/wait.h>
    #include <unistd.h>
#else
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>
#endif

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
#if !defined(LF_DISABLE_PLATFORM_ALLOCATOR)
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
#endif

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

// =============================================================================
// Static Buffer BlockPool Factory (BlockPool::from_buffer) Tests
// =============================================================================

TEST_CASE("BlockPool static buffer factory (from_buffer) alignment, capacity, and lifecycle", "[memory][buffer]") {
    SECTION("Unaligned buffer pointer realignment and exact slab capacity calculation") {
        constexpr usize BUFFER_SLABS = 3;
        constexpr usize BUFFER_SIZE = (BUFFER_SLABS + 1) * SLAB_SIZE;
        std::vector<std::byte> raw_buffer(BUFFER_SIZE);

        // Intentionally offset the buffer pointer so it is not 64KB aligned
        constexpr usize MISALIGNMENT_OFFSET = 128;
        void* unaligned_ptr = static_cast<void*>(raw_buffer.data() + MISALIGNMENT_OFFSET);
        usize raw_bytes = BUFFER_SIZE - MISALIGNMENT_OFFSET;

        auto raw_addr = reinterpret_cast<std::uintptr_t>(unaligned_ptr);
        auto aligned_addr = (raw_addr + (SLAB_SIZE - 1)) & ~(static_cast<std::uintptr_t>(SLAB_SIZE - 1));
        usize expected_offset = static_cast<usize>(aligned_addr - raw_addr);
        usize expected_slabs = (raw_bytes - expected_offset) / SLAB_SIZE;

        REQUIRE(expected_slabs >= BUFFER_SLABS);

        BlockPool pool = BlockPool::from_buffer(unaligned_ptr, raw_bytes);
        REQUIRE(pool.total_slabs() == expected_slabs);
        REQUIRE(pool.available_slabs() == expected_slabs);
        REQUIRE(pool.chunk_count() == 1);
        REQUIRE_FALSE(pool.callbacks().is_valid());

        // Verify all acquired slabs are strictly aligned to 64KB
        std::vector<Slab*> acquired;
        acquired.reserve(expected_slabs);
        for (usize i = 0; i < expected_slabs; ++i) {
            Slab* s = pool.acquire_slab();
            REQUIRE(s != nullptr);
            REQUIRE(reinterpret_cast<std::uintptr_t>(s) % SLAB_SIZE == 0);
            REQUIRE(reinterpret_cast<std::uintptr_t>(s->payload()) % CACHELINE_SIZE == 0);

            // Verify payload write and read
            std::memset(s->payload(), static_cast<int>(0x55 + i), Slab::PAYLOAD_SIZE);
            acquired.push_back(s);
        }

        REQUIRE(pool.available_slabs() == 0);

        // Verify data integrity
        for (usize i = 0; i < expected_slabs; ++i) {
            REQUIRE(acquired[i]->payload()[0] == static_cast<std::byte>(0x55 + i));
            pool.release_slab(acquired[i]);
        }
        REQUIRE(pool.available_slabs() == expected_slabs);
    }

    SECTION("Strict non-owning semantics: ~BlockPool() never calls free on static buffer") {
        CustomTrackingAllocator tracker{};
        MemoryCallbacks callbacks{
            .alloc = &test_custom_alloc,
            .free = &test_custom_free,
            .user_data = &tracker
        };

        constexpr usize STATIC_SLABS = 4;
        constexpr usize STATIC_BYTES = STATIC_SLABS * SLAB_SIZE;
        void* engine_buffer = callbacks.alloc(STATIC_BYTES, SLAB_SIZE, callbacks.user_data);
        REQUIRE(engine_buffer != nullptr);
        REQUIRE(tracker.alloc_calls == 1);
        REQUIRE(tracker.free_calls == 0);

        {
            BlockPool pool = BlockPool::from_buffer(engine_buffer, STATIC_BYTES);
            REQUIRE(pool.total_slabs() == STATIC_SLABS);
            REQUIRE(pool.available_slabs() == STATIC_SLABS);

            Slab* s1 = pool.acquire_slab();
            REQUIRE(s1 != nullptr);
            std::memset(s1->payload(), 0xAA, Slab::PAYLOAD_SIZE);
            pool.release_slab(s1);
        } // BlockPool destroyed here!

        // Non-owning guarantee: tracker free_calls must strictly remain 0
        REQUIRE(tracker.free_calls == 0);

        // Verify buffer memory was not corrupted by pool destruction
        auto* first_slab = reinterpret_cast<Slab*>(engine_buffer);
        REQUIRE(first_slab->payload()[0] == static_cast<std::byte>(0xAA));

        // Engine retains ownership and safely deallocates its buffer
        callbacks.free(engine_buffer, STATIC_BYTES, SLAB_SIZE, callbacks.user_data);
        REQUIRE(tracker.free_calls == 1);
    }

    SECTION("Strict fail-fast when buffer slabs are exhausted (growth permanently disabled)") {
        alignas(SLAB_SIZE) std::array<std::byte, SLAB_SIZE * 2> buffer{};
        BlockPool pool = BlockPool::from_buffer(buffer.data(), buffer.size());

        REQUIRE(pool.total_slabs() == 2);
        REQUIRE(pool.available_slabs() == 2);

        Slab* s1 = pool.acquire_slab();
        Slab* s2 = pool.acquire_slab();
        REQUIRE(s1 != nullptr);
        REQUIRE(s2 != nullptr);
        REQUIRE(pool.available_slabs() == 0);

        // Pool is exhausted: acquire_slab MUST return nullptr immediately without growing
        Slab* s3 = pool.acquire_slab();
        REQUIRE(s3 == nullptr);
        REQUIRE(pool.total_slabs() == 2);
        REQUIRE(pool.available_slabs() == 0);
        REQUIRE(pool.chunk_count() == 1);

        // Releasing a slab restores availability
        pool.release_slab(s1);
        REQUIRE(pool.available_slabs() == 1);

        Slab* s1_reacquired = pool.acquire_slab();
        REQUIRE(s1_reacquired == s1);
        REQUIRE(pool.available_slabs() == 0);
        REQUIRE(pool.acquire_slab() == nullptr);

        pool.release_slab(s1_reacquired);
        pool.release_slab(s2);
        REQUIRE(pool.available_slabs() == 2);
    }

    SECTION("SlabArena bump allocation and multi-slab chaining on static buffer pool") {
        alignas(SLAB_SIZE) std::array<std::byte, SLAB_SIZE * 2> buffer{};
        BlockPool pool = BlockPool::from_buffer(buffer.data(), buffer.size());

        {
            SlabArena arena(pool);

            // Bump allocation within first slab
            void* p1 = arena.allocate(1024, 64);
            REQUIRE(p1 != nullptr);
            REQUIRE(reinterpret_cast<std::uintptr_t>(p1) % 64 == 0);

            // Allocate large chunk to chain second slab
            constexpr usize ALLOC_SIZE = 40000;
            void* p2 = arena.allocate(ALLOC_SIZE, 64);
            REQUIRE(p2 != nullptr);
            void* p3 = arena.allocate(ALLOC_SIZE, 64);
            REQUIRE(p3 != nullptr);

            // Both slabs in the static pool are now acquired
            REQUIRE(pool.available_slabs() == 0);

            // Exhaustion in SlabArena when pool cannot grow returns nullptr
            void* p_overflow = arena.allocate(ALLOC_SIZE, 64);
            REQUIRE(p_overflow == nullptr);

            // Resetting arena releases chained slabs back to BlockPool in O(1)
            arena.reset();
            REQUIRE(pool.available_slabs() == 2);
        }
    }

    SECTION("BlockPool move construction transfers slabs and leaves source empty") {
        alignas(SLAB_SIZE) std::array<std::byte, SLAB_SIZE * 2> buffer{};
        BlockPool src = BlockPool::from_buffer(buffer.data(), buffer.size());
        REQUIRE(src.total_slabs() == 2);
        REQUIRE(src.available_slabs() == 2);

        BlockPool dst = std::move(src);
        REQUIRE(dst.total_slabs() == 2);
        REQUIRE(dst.available_slabs() == 2);
        REQUIRE(dst.chunk_count() == 1);

        // Source must be empty and inert
        REQUIRE(src.total_slabs() == 0);
        REQUIRE(src.available_slabs() == 0);
        REQUIRE(src.chunk_count() == 0);
        REQUIRE(src.acquire_slab() == nullptr);

        // Target can acquire and release normally
        Slab* s = dst.acquire_slab();
        REQUIRE(s != nullptr);
        REQUIRE(dst.available_slabs() == 1);
        dst.release_slab(s);
        REQUIRE(dst.available_slabs() == 2);
    }
}

// =============================================================================
// Global Allocator Bootstrap & TaskGraph Integration Tests
// =============================================================================

namespace {

struct ThreadSafeTracker {
    std::atomic<usize> alloc_calls{0};
    std::atomic<usize> free_calls{0};
    std::atomic<usize> bytes_allocated{0};
    std::atomic<usize> bytes_freed{0};
    std::atomic<usize> current_bytes{0};
};

void* ts_tracker_alloc(usize bytes, usize alignment, void* user_data) noexcept {
    auto* tracker = static_cast<ThreadSafeTracker*>(user_data);
    void* ptr = nullptr;
#if defined(_WIN32)
    ptr = _aligned_malloc(bytes, alignment);
#else
    int res = ::posix_memalign(&ptr, alignment, bytes);
    if (res != 0) {
        return nullptr;
    }
#endif
    if (ptr != nullptr && tracker != nullptr) {
        tracker->alloc_calls.fetch_add(1, std::memory_order_relaxed);
        tracker->bytes_allocated.fetch_add(bytes, std::memory_order_relaxed);
        tracker->current_bytes.fetch_add(bytes, std::memory_order_relaxed);
    }
    return ptr;
}

void ts_tracker_free(void* ptr, usize bytes, usize /*alignment*/, void* user_data) noexcept {
    if (ptr == nullptr) return;
    auto* tracker = static_cast<ThreadSafeTracker*>(user_data);
    if (tracker != nullptr) {
        tracker->free_calls.fetch_add(1, std::memory_order_relaxed);
        tracker->bytes_freed.fetch_add(bytes, std::memory_order_relaxed);
        tracker->current_bytes.fetch_sub(bytes, std::memory_order_relaxed);
    }
#if defined(_WIN32)
    _aligned_free(ptr);
#else
    ::free(ptr);
#endif
}

} // anonymous namespace

TEST_CASE("BlockPool global bootstrap and timing enforcement", "[memory][bootstrap]") {
    BlockPool::reset_global_for_testing();
    REQUIRE_FALSE(BlockPool::is_global_initialized());

    CustomTrackingAllocator tracker{};
    MemoryCallbacks custom{
        .alloc = &test_custom_alloc,
        .free = &test_custom_free,
        .user_data = &tracker
    };

    SECTION("Configuring global callbacks before initialization succeeds") {
        BlockPool::set_global_callbacks(custom);
        const MemoryCallbacks& cb = BlockPool::global_callbacks();
        REQUIRE(cb.alloc == &test_custom_alloc);
        REQUIRE(cb.free == &test_custom_free);
        REQUIRE(cb.user_data == &tracker);

        // Global pool is still not initialized until first access
        REQUIRE_FALSE(BlockPool::is_global_initialized());

        // First access to BlockPool::global() initializes the pool using custom callbacks
        BlockPool& global_pool = BlockPool::global();
        REQUIRE(BlockPool::is_global_initialized());
        REQUIRE(global_pool.callbacks().alloc == &test_custom_alloc);
        REQUIRE(global_pool.callbacks().user_data == &tracker);
        REQUIRE(tracker.alloc_calls >= 1);
        REQUIRE(tracker.total_bytes_allocated >= BlockPool::DEFAULT_INITIAL_SLABS * SLAB_SIZE);

        // Can acquire and release slabs through global pool
        Slab* s = global_pool.acquire_slab();
        REQUIRE(s != nullptr);
        global_pool.release_slab(s);

        // Calling set_global_callbacks AFTER initialization must trigger LF_ASSERT
#if !defined(_WIN32) && !defined(NDEBUG)
        pid_t pid = fork();
        REQUIRE(pid >= 0);
        if (pid == 0) {
            // Child process: restore default SIGABRT so Catch2 doesn't intercept it
            std::signal(SIGABRT, SIG_DFL);
            if (freopen("/dev/null", "w", stderr) == nullptr) {}
            if (freopen("/dev/null", "w", stdout) == nullptr) {}
            CustomTrackingAllocator dummy_tracker{};
            MemoryCallbacks dummy_cb{
                .alloc = &test_custom_alloc,
                .free = &test_custom_free,
                .user_data = &dummy_tracker
            };
            BlockPool::set_global_callbacks(dummy_cb);
            _exit(0);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        REQUIRE(WIFSIGNALED(status));
        REQUIRE(WTERMSIG(status) == SIGABRT);
#else
        REQUIRE(BlockPool::is_global_initialized());
#endif
    }

    // Teardown and reset
    BlockPool::reset_global_for_testing();
    REQUIRE_FALSE(BlockPool::is_global_initialized());
#if defined(LF_DISABLE_PLATFORM_ALLOCATOR)
    lf::test::install_test_allocator();
#endif
}

TEST_CASE("Custom allocator integration: multi-stage TaskGraph with parallelFor and condition branching", "[memory][integration]") {
    BlockPool::reset_global_for_testing();

    ThreadSafeTracker tracker{};
    MemoryCallbacks custom{
        .alloc = &ts_tracker_alloc,
        .free = &ts_tracker_free,
        .user_data = &tracker
    };

    BlockPool::set_global_callbacks(custom);

    {
        SchedulerConfig config{.workerCount = 4, .initialDequeCapacity = 2048};
        TaskScheduler scheduler(config);

        constexpr usize DATA_COUNT = 5000;
        std::vector<u32> data(DATA_COUNT, 0);

        TaskGraph graph;
        std::atomic<bool> conditionBranchTaken{false};
        std::atomic<usize> subflowCount{0};

        // Stage 1: ParallelFor populating data array
        auto parallelStage = graph.parallelFor("InitData", DATA_COUNT, 250, [&data](usize i) noexcept {
            data[i] = static_cast<u32>(i * 3);
        });

        // Stage 2: Condition task checking if first element is 0 (branch 0)
        auto conditionNode = graph.emplaceCondition("CheckData", [&data]() noexcept -> int {
            return (data[0] == 0) ? 0 : 1;
        });
        parallelStage.precede(conditionNode);

        // Stage 3: Dynamic Subflow executed on Branch 0
        auto subflowTask = graph.emplaceSubflow("DynamicSubflow", [&](Subflow& sf) {
            conditionBranchTaken.store(true, std::memory_order_release);
            for (usize i = 0; i < 10; ++i) {
                sf.emplace([&subflowCount, i, &data]() noexcept {
                    for (usize j = i * 500; j < (i + 1) * 500; ++j) {
                        data[j] += 1;
                    }
                    subflowCount.fetch_add(1, std::memory_order_release);
                });
            }
        });

        // Alternative branch 1 if condition failed (should be skipped)
        auto fallbackTask = graph.emplace("Fallback", []() noexcept {
            // Should not be executed
        });

        // Stage 4: Barrier verifying all results
        std::atomic<bool> verificationPassed{false};
        auto barrier = graph.emplace("Barrier", [&]() noexcept {
            bool correct = true;
            for (usize i = 0; i < DATA_COUNT; ++i) {
                u32 expected = static_cast<u32>(i * 3 + 1);
                if (data[i] != expected) {
                    correct = false;
                    break;
                }
            }
            verificationPassed.store(correct, std::memory_order_release);
        });

        conditionNode.to(0) >> subflowTask >> barrier;
        conditionNode.to(1) >> fallbackTask >> barrier;

        Status status = scheduler.runAndWait(graph);
        REQUIRE(status == Status::Success);
        REQUIRE(graph.isCompleted());
        REQUIRE(conditionBranchTaken.load(std::memory_order_acquire));
        REQUIRE(subflowCount.load(std::memory_order_acquire) == 10);
        REQUIRE(verificationPassed.load(std::memory_order_acquire));

        // Verify telemetry from custom tracking allocator
        REQUIRE(tracker.alloc_calls.load(std::memory_order_relaxed) > 0);
        REQUIRE(tracker.bytes_allocated.load(std::memory_order_relaxed) >= BlockPool::DEFAULT_INITIAL_SLABS * SLAB_SIZE);
    }

    // After scheduler and graph destruction, reset global pool and verify clean deallocation
    BlockPool::reset_global_for_testing();
    REQUIRE(tracker.bytes_freed.load(std::memory_order_relaxed) == tracker.bytes_allocated.load(std::memory_order_relaxed));
    REQUIRE(tracker.free_calls.load(std::memory_order_relaxed) == tracker.alloc_calls.load(std::memory_order_relaxed));
    REQUIRE(tracker.current_bytes.load(std::memory_order_relaxed) == 0);

#if defined(LF_DISABLE_PLATFORM_ALLOCATOR)
    lf::test::install_test_allocator();
#endif
}

// =============================================================================
// Documentation Engine Recipes Verification (docs/api/memory.md)
// =============================================================================

namespace doc_recipes {

// Recipe 1: Dynamic OS Virtual Memory Provider
inline void* os_virtual_alloc(lf::usize bytes, lf::usize alignment, void* /*user_data*/) noexcept {
#if defined(_WIN32)
    (void)alignment;
    return ::VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    #if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
        #define MAP_ANONYMOUS MAP_ANON
    #endif

    const long sys_page = ::sysconf(_SC_PAGESIZE);
    const lf::usize page_size = (sys_page > 0) ? static_cast<lf::usize>(sys_page) : 4096;

    if (alignment <= page_size) {
        void* ptr = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        return (ptr == MAP_FAILED) ? nullptr : ptr;
    }

    const lf::usize total_reserve = bytes + alignment;
    void* raw = ::mmap(nullptr, total_reserve, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) {
        return nullptr;
    }

    const auto raw_addr = reinterpret_cast<std::uintptr_t>(raw);
    const auto aligned_addr = (raw_addr + alignment - 1) & ~(alignment - 1);
    const lf::usize prefix = aligned_addr - raw_addr;
    const lf::usize suffix = total_reserve - prefix - bytes;

    if (prefix > 0) {
        ::munmap(raw, prefix);
    }
    if (suffix > 0) {
        ::munmap(reinterpret_cast<void*>(aligned_addr + bytes), suffix);
    }

    return reinterpret_cast<void*>(aligned_addr);
#endif
}

inline void os_virtual_free(void* ptr, lf::usize bytes, lf::usize /*alignment*/, void* /*user_data*/) noexcept {
    if (ptr == nullptr) return;
#if defined(_WIN32)
    (void)bytes;
    ::VirtualFree(ptr, 0, MEM_RELEASE);
#else
    ::munmap(ptr, bytes);
#endif
}

inline lf::MemoryCallbacks make_virtual_memory_callbacks() noexcept {
    return lf::MemoryCallbacks{
        .alloc = &os_virtual_alloc,
        .free = &os_virtual_free,
        .user_data = nullptr
    };
}

// Recipe 3: Subsystem Memory Telemetry & Budget Enforcement Tracker
struct SubsystemBudgetTracker {
    const char* name{nullptr};
    lf::usize budget_bytes{0};
    std::atomic<lf::usize> current_bytes{0};
    std::atomic<lf::usize> peak_bytes{0};
    std::atomic<lf::usize> alloc_count{0};
    std::atomic<lf::usize> free_count{0};
    lf::MemoryCallbacks backing_callbacks{};
};

inline void* budgeted_alloc(lf::usize bytes, lf::usize alignment, void* user_data) noexcept {
    auto* tracker = static_cast<SubsystemBudgetTracker*>(user_data);
    if (tracker == nullptr) return nullptr;

    lf::usize current = tracker->current_bytes.load(std::memory_order_relaxed);
    while (true) {
        if (current + bytes > tracker->budget_bytes) {
            return nullptr;
        }
        if (tracker->current_bytes.compare_exchange_weak(current, current + bytes,
                                                         std::memory_order_acq_rel,
                                                         std::memory_order_relaxed)) {
            break;
        }
    }

    void* ptr = tracker->backing_callbacks.alloc(bytes, alignment, tracker->backing_callbacks.user_data);
    if (ptr == nullptr) {
        tracker->current_bytes.fetch_sub(bytes, std::memory_order_relaxed);
        return nullptr;
    }

    tracker->alloc_count.fetch_add(1, std::memory_order_relaxed);
    lf::usize new_bytes = current + bytes;
    lf::usize prev_peak = tracker->peak_bytes.load(std::memory_order_relaxed);
    while (new_bytes > prev_peak &&
           !tracker->peak_bytes.compare_exchange_weak(prev_peak, new_bytes,
                                                      std::memory_order_relaxed)) {
    }

    return ptr;
}

inline void budgeted_free(void* ptr, lf::usize bytes, lf::usize alignment, void* user_data) noexcept {
    if (ptr == nullptr) return;
    auto* tracker = static_cast<SubsystemBudgetTracker*>(user_data);
    if (tracker == nullptr) return;

    tracker->backing_callbacks.free(ptr, bytes, alignment, tracker->backing_callbacks.user_data);
    tracker->current_bytes.fetch_sub(bytes, std::memory_order_relaxed);
    tracker->free_count.fetch_add(1, std::memory_order_relaxed);
}

} // namespace doc_recipes

TEST_CASE("Doc Recipe: Dynamic OS Virtual Memory (mmap/VirtualAlloc)", "[memory][recipe]") {
    lf::MemoryCallbacks callbacks = doc_recipes::make_virtual_memory_callbacks();
    REQUIRE(callbacks.is_valid());

    // Create pool backed by OS virtual memory pages
    constexpr lf::usize INITIAL_SLABS = 4;
    constexpr lf::usize CHUNK_SLABS = 4;
    lf::BlockPool pool(callbacks, INITIAL_SLABS, CHUNK_SLABS, true);

    REQUIRE(pool.total_slabs() == INITIAL_SLABS);
    REQUIRE(pool.available_slabs() == INITIAL_SLABS);
    REQUIRE(pool.chunk_count() == 1);

    // Run TaskGraph on this pool
    {
        lf::TaskGraph graph(&pool);
        std::atomic<int> counter{0};

        auto t1 = graph.emplace("Init", [&counter]() noexcept {
            counter.fetch_add(1, std::memory_order_relaxed);
        });
        auto t2 = graph.emplace("Compute", [&counter]() noexcept {
            counter.fetch_add(2, std::memory_order_relaxed);
        });
        t1.precede(t2);

        lf::SchedulerConfig config{.workerCount = 2};
        lf::TaskScheduler scheduler(config);
        lf::Status status = scheduler.runAndWait(graph);

        REQUIRE(status == lf::Status::Success);
        REQUIRE(counter.load() == 3);
    }

    // Dynamic growth: acquire all initial slabs plus an extra chunk
    std::vector<lf::Slab*> slabs;
    for (lf::usize i = 0; i < INITIAL_SLABS + 1; ++i) {
        lf::Slab* s = pool.acquire_slab();
        REQUIRE(s != nullptr);
        slabs.push_back(s);
    }
    REQUIRE(pool.chunk_count() == 2);
    REQUIRE(pool.total_slabs() == INITIAL_SLABS + CHUNK_SLABS);

    for (lf::Slab* s : slabs) {
        pool.release_slab(s);
    }
    REQUIRE(pool.available_slabs() == pool.total_slabs());
}

TEST_CASE("Doc Recipe: Pre-allocated Static Arena Buffer (from_buffer)", "[memory][recipe]") {
    constexpr lf::usize STATIC_SLABS = 8;
    constexpr lf::usize BUFFER_BYTES = lf::SLAB_SIZE * STATIC_SLABS;
    void* renderBuffer = doc_recipes::os_virtual_alloc(BUFFER_BYTES, lf::SLAB_SIZE, nullptr);
    REQUIRE(renderBuffer != nullptr);

    // Create non-owning BlockPool from pre-allocated buffer
    {
        lf::BlockPool pool = lf::BlockPool::from_buffer(renderBuffer, BUFFER_BYTES);
        REQUIRE(pool.total_slabs() == STATIC_SLABS);
        REQUIRE(pool.available_slabs() == STATIC_SLABS);
        REQUIRE(pool.chunk_count() == 1);

        lf::TaskGraph graph(&pool);
        std::atomic<int> sum{0};

        auto parallelStage = graph.parallelFor("BufferWork", 1000, 100, [&sum](lf::usize) noexcept {
            sum.fetch_add(1, std::memory_order_relaxed);
        });

        auto verifyStage = graph.emplace("Verify", [&sum]() noexcept {
            (void)sum.load(std::memory_order_relaxed);
        });
        parallelStage.precede(verifyStage);

        lf::SchedulerConfig config{.workerCount = 2};
        lf::TaskScheduler scheduler(config);
        lf::Status status = scheduler.runAndWait(graph);

        REQUIRE(status == lf::Status::Success);
        REQUIRE(sum.load() == 1000);
    }

    // Buffer remains owned by engine; pool destructs cleanly without OS deallocations.
    // Engine frees its own buffer when appropriate:
    doc_recipes::os_virtual_free(renderBuffer, BUFFER_BYTES, lf::SLAB_SIZE, nullptr);
}

TEST_CASE("Doc Recipe: Subsystem Memory Telemetry and Budget Enforcement", "[memory][recipe]") {
    constexpr lf::usize BUDGET_SLABS = 4;
    constexpr lf::usize BUDGET_BYTES = BUDGET_SLABS * lf::SLAB_SIZE;

    doc_recipes::SubsystemBudgetTracker tracker{
        .name = "RendererSubsystem",
        .budget_bytes = BUDGET_BYTES,
        .backing_callbacks = doc_recipes::make_virtual_memory_callbacks()
    };

    lf::MemoryCallbacks callbacks{
        .alloc = &doc_recipes::budgeted_alloc,
        .free = &doc_recipes::budgeted_free,
        .user_data = &tracker
    };

    // Initialize pool with 2 slabs initially and 2 slabs per chunk
    constexpr lf::usize INITIAL_SLABS = 2;
    constexpr lf::usize CHUNK_SLABS = 2;
    {
        lf::BlockPool pool(callbacks, INITIAL_SLABS, CHUNK_SLABS, true);
        REQUIRE(pool.total_slabs() == INITIAL_SLABS);
        REQUIRE(tracker.alloc_count.load() == 1);
        REQUIRE(tracker.current_bytes.load() == INITIAL_SLABS * lf::SLAB_SIZE);
        REQUIRE(tracker.peak_bytes.load() == INITIAL_SLABS * lf::SLAB_SIZE);

        // Execute task graph on budgeted pool
        {
            lf::TaskGraph graph(&pool);
            std::atomic<bool> executed{false};
            graph.emplace("BudgetedTask", [&executed]() noexcept {
                executed.store(true, std::memory_order_release);
            });

            lf::SchedulerConfig config{.workerCount = 2};
            lf::TaskScheduler scheduler(config);
            lf::Status status = scheduler.runAndWait(graph);
            REQUIRE(status == lf::Status::Success);
            REQUIRE(executed.load(std::memory_order_acquire));
            graph.clear();
        }

        // With graph cleared, all slabs are back in pool
        REQUIRE(pool.available_slabs() == INITIAL_SLABS);

        // Slabs are acquired:
        // First 2 come from initial chunk.
        // 3rd slab triggers chunk growth: +2 slabs (total = 4 slabs = 100% budget reached).
        std::vector<lf::Slab*> slabs;
        for (lf::usize i = 0; i < BUDGET_SLABS; ++i) {
            lf::Slab* s = pool.acquire_slab();
            REQUIRE(s != nullptr);
            slabs.push_back(s);
        }
        REQUIRE(pool.total_slabs() == BUDGET_SLABS);
        REQUIRE(pool.available_slabs() == 0);
        REQUIRE(tracker.alloc_count.load() == 2);
        REQUIRE(tracker.current_bytes.load() == BUDGET_BYTES);
        REQUIRE(tracker.peak_bytes.load() == BUDGET_BYTES);

        // Next chunk allocation would exceed budget: acquire must return nullptr (strict fail-fast)
        lf::Slab* over_budget_slab = pool.acquire_slab();
        REQUIRE(over_budget_slab == nullptr);

        for (lf::Slab* s : slabs) {
            pool.release_slab(s);
        }
    }

    // After pool destruction, all memory is cleanly returned and telemetry balances
    REQUIRE(tracker.current_bytes.load() == 0);
    REQUIRE(tracker.free_count.load() == tracker.alloc_count.load());
    REQUIRE(tracker.peak_bytes.load() == BUDGET_BYTES);
}


