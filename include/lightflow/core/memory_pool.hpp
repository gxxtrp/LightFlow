#pragma once

#include <lightflow/core/types.hpp>
#include <lightflow/core/tracy.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace lf {

/// Header placed at the beginning of each 64KB slab.
/// Aligned to 64 bytes (CACHELINE_SIZE) so that payload immediately following it
/// is also naturally 64-byte cacheline aligned.
struct LF_ALIGN_CACHELINE Slab {
    std::atomic<Slab*> next{nullptr};

    Slab() noexcept : next(nullptr) {}
    explicit Slab(Slab* n) noexcept : next(n) {}

    static constexpr usize HEADER_SIZE = CACHELINE_SIZE;
    static constexpr usize PAYLOAD_SIZE = SLAB_SIZE - HEADER_SIZE;

    LF_NODISCARD std::byte* payload() noexcept {
        return reinterpret_cast<std::byte*>(this) + HEADER_SIZE;
    }

    LF_NODISCARD const std::byte* payload() const noexcept {
        return reinterpret_cast<const std::byte*>(this) + HEADER_SIZE;
    }
};

static_assert(sizeof(Slab) == CACHELINE_SIZE, "Slab header must occupy exactly 1 cacheline (64 bytes)");
static_assert(alignof(Slab) == CACHELINE_SIZE, "Slab must be aligned to CACHELINE_SIZE");
static_assert(Slab::PAYLOAD_SIZE == SLAB_SIZE - CACHELINE_SIZE, "Payload size calculation mismatch");

/// 128-bit tagged pointer for ABA-safe lock-free atomic stack operations.
struct alignas(16) TaggedSlab {
    Slab* ptr{nullptr};
    u64 tag{0};

    bool operator==(const TaggedSlab& other) const noexcept = default;
};

/// C-style function pointer callbacks for virtual memory chunk allocation and deallocation.
/// Guarantees zero virtual dispatch, binary-stable across DLL boundaries, and -fno-rtti compatible.
struct MemoryCallbacks {
    using AllocFn = void* (*)(usize bytes, usize alignment, void* user_data) noexcept;
    using FreeFn = void (*)(void* ptr, usize bytes, usize alignment, void* user_data) noexcept;

    AllocFn alloc{nullptr};
    FreeFn free{nullptr};
    void* user_data{nullptr};

    LF_NODISCARD constexpr bool is_valid() const noexcept {
        return alloc != nullptr && free != nullptr;
    }
};

#if !defined(LF_DISABLE_PLATFORM_ALLOCATOR)
/// Returns the platform default MemoryCallbacks using posix_memalign / _aligned_malloc.
LF_NODISCARD MemoryCallbacks platform_memory_callbacks() noexcept;
#endif

/// Central lock-free 64KB memory block pool.
/// Manages virtual memory chunks divided into 64KB slabs with an atomic freelist stack.
class alignas(CACHELINE_SIZE) BlockPool {
public:
    static constexpr usize DEFAULT_INITIAL_SLABS = 256; // 16 MB
    static constexpr usize DEFAULT_CHUNK_SLABS = 256;   // 16 MB

    explicit BlockPool(usize initial_slabs = DEFAULT_INITIAL_SLABS,
                       usize chunk_slabs = DEFAULT_CHUNK_SLABS,
                       bool allow_growth = true) noexcept;

    explicit BlockPool(const MemoryCallbacks& callbacks,
                       usize initial_slabs = DEFAULT_INITIAL_SLABS,
                       usize chunk_slabs = DEFAULT_CHUNK_SLABS,
                       bool allow_growth = true) noexcept;

    ~BlockPool() noexcept;

    /// Returns the global default BlockPool instance.
    LF_NODISCARD static BlockPool& global() noexcept;

    /// Configures the global MemoryCallbacks used when BlockPool::global() is initialized.
    /// Must be called during engine bootstrap BEFORE BlockPool::global() is first accessed or allocated.
    /// Asserts (LF_ASSERT) if called after BlockPool::global() has already been initialized.
    static void set_global_callbacks(const MemoryCallbacks& callbacks) noexcept;

    /// Returns the global MemoryCallbacks configured for the engine bootstrap.
    LF_NODISCARD static const MemoryCallbacks& global_callbacks() noexcept;

    /// Returns true if BlockPool::global() has already been initialized.
    LF_NODISCARD static bool is_global_initialized() noexcept;

    /// Testing-only helper to destruct BlockPool::global() and reset global bootstrap state.
    static void reset_global_for_testing() noexcept;

    /// Creates a BlockPool backed by a pre-allocated static contiguous memory buffer.
    /// The buffer is partitioned into 64KB slabs aligned to 64KB. Dynamic growth is permanently disabled.
    /// Non-owning semantics: the buffer memory is never deallocated on pool destruction.
    LF_NODISCARD static BlockPool from_buffer(void* buffer, usize bytes) noexcept;

    // Move constructible
    BlockPool(BlockPool&& other) noexcept;

    // Non-copyable, non-move-assignable
    BlockPool(const BlockPool&) = delete;
    BlockPool& operator=(const BlockPool&) = delete;
    BlockPool& operator=(BlockPool&&) = delete;

    /// Acquires a single 64KB slab from the pool freelist.
    /// Thread-safe and lock-free on the fast path.
    LF_NODISCARD Slab* acquire_slab() noexcept;

    /// Releases a single slab back to the pool freelist.
    /// Thread-safe and lock-free.
    void release_slab(Slab* slab) noexcept;

    /// Releases a linked chain of slabs (from head to tail) back to the freelist in O(1).
    /// Thread-safe and lock-free (single atomic CAS).
    void release_slab_chain(Slab* head, Slab* tail, usize count) noexcept;

    /// Returns the number of currently available slabs in the pool.
    LF_NODISCARD usize available_slabs() const noexcept;

    /// Returns the total number of slabs managed across all chunks.
    LF_NODISCARD usize total_slabs() const noexcept;

    /// Returns the number of contiguous virtual memory chunks currently allocated.
    LF_NODISCARD usize chunk_count() const noexcept;

    /// Returns the memory callbacks configured for this BlockPool.
    LF_NODISCARD const MemoryCallbacks& callbacks() const noexcept {
        return m_callbacks;
    }

private:
    struct Chunk {
        void* base_ptr{nullptr};
        usize slab_count{0};
        Chunk* next{nullptr};
        bool is_owned{true};
    };

    explicit BlockPool(void* buffer, usize bytes) noexcept;

    /// Allocates an OS-level chunk of contiguous 64KB slabs and pushes them to the freelist.
    Slab* allocate_chunk_and_acquire() noexcept;

    // Dedicated cacheline for the lock-free freelist stack head
    alignas(CACHELINE_SIZE) std::atomic<TaggedSlab> m_head{TaggedSlab{nullptr, 0}};

    // Dedicated cacheline for the atomic available slab counter
    alignas(CACHELINE_SIZE) std::atomic<usize> m_available_slabs{0};

    // Cold metadata
    alignas(CACHELINE_SIZE) std::atomic<usize> m_total_slabs{0};
    std::atomic<usize> m_chunk_count{0};
    usize m_chunk_slabs{DEFAULT_CHUNK_SLABS};
    bool m_allow_growth{true};
    MemoryCallbacks m_callbacks{};

    // Synchronized chunk list for RAII cleanup
    std::atomic<Chunk*> m_chunks{nullptr};
    std::atomic<bool> m_growth_lock{false};
};

} // namespace lf
