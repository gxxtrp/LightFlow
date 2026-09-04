#pragma once

#include <lightflow/core/types.hpp>
#include <lightflow/core/tracy.hpp>
#include <lightflow/core/memory_pool.hpp>

#include <cstddef>
#include <memory_resource>
#include <new>
#include <span>
#include <utility>

namespace lf {

class SlabArena;

/// Adapter conforming to std::pmr::memory_resource backed by an lf::SlabArena.
class SlabMemoryResource : public std::pmr::memory_resource {
public:
    explicit SlabMemoryResource(SlabArena& arena) noexcept
        : m_arena(&arena) {}

    LF_NODISCARD SlabArena& arena() noexcept { return *m_arena; }
    LF_NODISCARD const SlabArena& arena() const noexcept { return *m_arena; }

protected:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override;
    void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) noexcept override;
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override;

private:
    friend class SlabArena;
    SlabArena* m_arena;
};

/// Thread-local wait-free bump-pointer arena allocator.
/// Automatically chains 64KB slabs from BlockPool when capacity is exceeded.
/// Reclaims all chained slabs back to BlockPool in O(1) via reset().
class SlabArena {
public:
    explicit SlabArena(BlockPool& pool) noexcept;
    ~SlabArena() noexcept;

    // Move semantics
    SlabArena(SlabArena&& other) noexcept;
    SlabArena& operator=(SlabArena&& other) noexcept;

    // Non-copyable
    SlabArena(const SlabArena&) = delete;
    SlabArena& operator=(const SlabArena&) = delete;

    /// Allocates memory aligned to the requested alignment (defaulting to 64 bytes).
    /// Wait-free fast path for thread-local use.
    LF_NODISCARD void* allocate(usize bytes, usize alignment = CACHELINE_SIZE) noexcept;

    /// No-op in bump arena; deallocation occurs in bulk at reset().
    void deallocate(void* ptr, usize bytes, usize alignment = CACHELINE_SIZE) noexcept;

    /// Constructs an object in-place inside the arena with guaranteed alignment.
    template <typename T, typename... Args>
    LF_NODISCARD T* create(Args&&... args) noexcept {
        void* mem = allocate(sizeof(T), alignof(T));
        if (mem == nullptr) [[unlikely]] {
            return nullptr;
        }
        return ::new (mem) T(std::forward<Args>(args)...);
    }

    /// Allocates a contiguous slice of typed elements inside the arena.
    template <typename T>
    LF_NODISCARD std::span<T> allocate_span(usize count) noexcept {
        if (count == 0) [[unlikely]] {
            return {};
        }
        void* mem = allocate(sizeof(T) * count, alignof(T));
        if (mem == nullptr) [[unlikely]] {
            return {};
        }
        return std::span<T>(static_cast<T*>(mem), count);
    }

    /// Reclaims all chained slabs back to the central BlockPool in O(1).
    void reset() noexcept;

    /// Returns the number of 64KB slabs currently chained to this arena.
    LF_NODISCARD usize slab_count() const noexcept { return m_slab_count; }

    /// Returns total user bytes allocated since creation or last reset.
    LF_NODISCARD usize total_allocated_bytes() const noexcept { return m_allocated_bytes; }

    /// Returns a std::pmr::memory_resource adapter wrapping this arena.
    LF_NODISCARD std::pmr::memory_resource* resource() noexcept { return &m_resource; }

    /// Returns the central pool backing this arena.
    LF_NODISCARD BlockPool* pool() const noexcept { return m_pool; }

private:
    struct OversizedBlock {
        void* raw_ptr{nullptr};
        OversizedBlock* next{nullptr};
    };

    void* allocate_slow(usize bytes, usize alignment) noexcept;

    BlockPool* m_pool{nullptr};
    Slab* m_head_slab{nullptr};
    Slab* m_tail_slab{nullptr};
    Slab* m_current_slab{nullptr};
    std::byte* m_bump_ptr{nullptr};
    std::byte* m_bump_end{nullptr};
    usize m_slab_count{0};
    usize m_allocated_bytes{0};

    OversizedBlock* m_oversized_head{nullptr};
    SlabMemoryResource m_resource;
};

} // namespace lf
