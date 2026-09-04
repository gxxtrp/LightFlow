#include <lightflow/core/memory_pool.hpp>

#include <cstdlib>
#include <new>

#if defined(_WIN32)
    #include <malloc.h>
#else
    #include <stdlib.h>
#endif

#if defined(__x86_64__) || defined(_M_X64)
    #include <immintrin.h>
#endif

namespace lf {

#if !defined(LF_DISABLE_PLATFORM_ALLOCATOR)
namespace {

void* platform_alloc(usize bytes, usize alignment, void* /*user_data*/) noexcept {
#if defined(_WIN32)
    return _aligned_malloc(bytes, alignment);
#else
    void* ptr = nullptr;
    int res = ::posix_memalign(&ptr, alignment, bytes);
    if (res != 0) {
        return nullptr;
    }
    return ptr;
#endif
}

void platform_free(void* ptr, usize /*bytes*/, usize /*alignment*/, void* /*user_data*/) noexcept {
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

MemoryCallbacks platform_memory_callbacks() noexcept {
    return MemoryCallbacks{
        .alloc = &platform_alloc,
        .free = &platform_free,
        .user_data = nullptr
    };
}

BlockPool::BlockPool(usize initial_slabs, usize chunk_slabs, bool allow_growth) noexcept
    : BlockPool(platform_memory_callbacks(), initial_slabs, chunk_slabs, allow_growth) {}
#else
BlockPool::BlockPool(usize initial_slabs, usize chunk_slabs, bool allow_growth) noexcept
    : BlockPool(MemoryCallbacks{}, initial_slabs, chunk_slabs, allow_growth) {}
#endif

BlockPool::BlockPool(const MemoryCallbacks& callbacks,
                     usize initial_slabs,
                     usize chunk_slabs,
                     bool allow_growth) noexcept
    : m_chunk_slabs(chunk_slabs > 0 ? chunk_slabs : DEFAULT_CHUNK_SLABS),
      m_allow_growth(allow_growth),
      m_callbacks(callbacks) {
    LF_ASSERT((callbacks.alloc == nullptr && callbacks.free == nullptr) ||
              (callbacks.alloc != nullptr && callbacks.free != nullptr));

    if (!m_callbacks.is_valid()) {
        m_allow_growth = false;
        return;
    }

    if (initial_slabs > 0) {
        usize total_bytes = initial_slabs * SLAB_SIZE;
        void* raw_mem = m_callbacks.alloc(total_bytes, SLAB_SIZE, m_callbacks.user_data);

        if (raw_mem != nullptr) {
            auto* chunk = new (std::nothrow) Chunk{raw_mem, initial_slabs, nullptr, true};
            if (chunk == nullptr) {
                m_callbacks.free(raw_mem, total_bytes, SLAB_SIZE, m_callbacks.user_data);
                return;
            }
            m_chunks.store(chunk, std::memory_order_relaxed);

            auto* base_bytes = static_cast<std::byte*>(raw_mem);
            for (usize i = 0; i < initial_slabs; ++i) {
                auto* current = reinterpret_cast<Slab*>(base_bytes + (i * SLAB_SIZE));
                auto* next = (i + 1 < initial_slabs)
                    ? reinterpret_cast<Slab*>(base_bytes + ((i + 1) * SLAB_SIZE))
                    : nullptr;
                current->next.store(next, std::memory_order_relaxed);
            }

            auto* first_slab = reinterpret_cast<Slab*>(base_bytes);
            m_head.store(TaggedSlab{first_slab, 0}, std::memory_order_release);
            m_available_slabs.store(initial_slabs, std::memory_order_release);
            m_total_slabs.store(initial_slabs, std::memory_order_release);
            m_chunk_count.store(1, std::memory_order_release);
        }
    }
}

BlockPool BlockPool::from_buffer(void* buffer, usize bytes) noexcept {
    return BlockPool(buffer, bytes);
}

BlockPool::BlockPool(void* buffer, usize bytes) noexcept
    : m_chunk_slabs(0),
      m_allow_growth(false),
      m_callbacks(MemoryCallbacks{}) {
    auto raw_addr = reinterpret_cast<std::uintptr_t>(buffer);
    std::uintptr_t aligned_addr = 0;
    usize offset = 0;
    usize usable_bytes = 0;
    usize slab_count = 0;

    if (buffer != nullptr) {
        aligned_addr = (raw_addr + (SLAB_SIZE - 1)) & ~(static_cast<std::uintptr_t>(SLAB_SIZE - 1));
        offset = static_cast<usize>(aligned_addr - raw_addr);
        usable_bytes = (bytes >= offset) ? (bytes - offset) : 0;
        slab_count = usable_bytes / SLAB_SIZE;
    }

    LF_ASSERT(buffer != nullptr && usable_bytes >= SLAB_SIZE);

    if (slab_count == 0) {
        return;
    }

    void* aligned_ptr = reinterpret_cast<void*>(aligned_addr);
    auto* base_bytes = static_cast<std::byte*>(aligned_ptr);
    for (usize i = 0; i < slab_count; ++i) {
        auto* current = reinterpret_cast<Slab*>(base_bytes + (i * SLAB_SIZE));
        auto* next = (i + 1 < slab_count)
            ? reinterpret_cast<Slab*>(base_bytes + ((i + 1) * SLAB_SIZE))
            : nullptr;
        current->next.store(next, std::memory_order_relaxed);
    }

    auto* chunk = new (std::nothrow) Chunk{
        .base_ptr = aligned_ptr,
        .slab_count = slab_count,
        .next = nullptr,
        .is_owned = false
    };
    if (chunk != nullptr) {
        m_chunks.store(chunk, std::memory_order_relaxed);
    }

    auto* first_slab = reinterpret_cast<Slab*>(base_bytes);
    m_head.store(TaggedSlab{first_slab, 0}, std::memory_order_release);
    m_available_slabs.store(slab_count, std::memory_order_release);
    m_total_slabs.store(slab_count, std::memory_order_release);
    m_chunk_count.store(1, std::memory_order_release);
}

BlockPool::BlockPool(BlockPool&& other) noexcept
    : m_chunk_slabs(other.m_chunk_slabs),
      m_allow_growth(other.m_allow_growth),
      m_callbacks(other.m_callbacks) {
    m_head.store(other.m_head.load(std::memory_order_relaxed), std::memory_order_relaxed);
    m_available_slabs.store(other.m_available_slabs.load(std::memory_order_relaxed), std::memory_order_relaxed);
    m_total_slabs.store(other.m_total_slabs.load(std::memory_order_relaxed), std::memory_order_relaxed);
    m_chunk_count.store(other.m_chunk_count.load(std::memory_order_relaxed), std::memory_order_relaxed);
    m_chunks.store(other.m_chunks.load(std::memory_order_relaxed), std::memory_order_relaxed);
    m_growth_lock.store(false, std::memory_order_relaxed);

    other.m_head.store(TaggedSlab{nullptr, 0}, std::memory_order_relaxed);
    other.m_available_slabs.store(0, std::memory_order_relaxed);
    other.m_total_slabs.store(0, std::memory_order_relaxed);
    other.m_chunk_count.store(0, std::memory_order_relaxed);
    other.m_chunks.store(nullptr, std::memory_order_relaxed);
    other.m_allow_growth = false;
}

BlockPool::~BlockPool() noexcept {
    Chunk* current = m_chunks.load(std::memory_order_relaxed);
    while (current != nullptr) {
        Chunk* next = current->next;
        if (current->is_owned && m_callbacks.free != nullptr && current->base_ptr != nullptr) {
            m_callbacks.free(current->base_ptr, current->slab_count * SLAB_SIZE, SLAB_SIZE, m_callbacks.user_data);
        }
        delete current;
        current = next;
    }
}

Slab* BlockPool::acquire_slab() noexcept {
    LF_ZONE_NAMED(zone, "BlockPool::acquire_slab");
    TaggedSlab current = m_head.load(std::memory_order_acquire);
    while (current.ptr != nullptr) {
        Slab* next_ptr = current.ptr->next.load(std::memory_order_relaxed);
        TaggedSlab next_head{next_ptr, current.tag + 1};
        if (m_head.compare_exchange_weak(current, next_head,
                                         std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
            m_available_slabs.fetch_sub(1, std::memory_order_relaxed);
            current.ptr->next.store(nullptr, std::memory_order_relaxed);
            return current.ptr;
        }
    }

    if (m_allow_growth) {
        return allocate_chunk_and_acquire();
    }
    return nullptr;
}

void BlockPool::release_slab(Slab* slab) noexcept {
    LF_ASSERT(slab != nullptr);
    release_slab_chain(slab, slab, 1);
}

void BlockPool::release_slab_chain(Slab* head, Slab* tail, usize count) noexcept {
    LF_ZONE_NAMED(zone, "BlockPool::release_slab_chain");
    if (head == nullptr || tail == nullptr || count == 0) {
        return;
    }

    TaggedSlab current = m_head.load(std::memory_order_relaxed);
    TaggedSlab new_head{head, 0};
    do {
        tail->next.store(current.ptr, std::memory_order_relaxed);
        new_head.tag = current.tag + 1;
    } while (!m_head.compare_exchange_weak(current, new_head,
                                           std::memory_order_release,
                                           std::memory_order_relaxed));

    m_available_slabs.fetch_add(count, std::memory_order_relaxed);
}

usize BlockPool::available_slabs() const noexcept {
    return m_available_slabs.load(std::memory_order_relaxed);
}

usize BlockPool::total_slabs() const noexcept {
    return m_total_slabs.load(std::memory_order_relaxed);
}

usize BlockPool::chunk_count() const noexcept {
    return m_chunk_count.load(std::memory_order_relaxed);
}

Slab* BlockPool::allocate_chunk_and_acquire() noexcept {
    LF_ZONE_NAMED(zone, "BlockPool::allocate_chunk_and_acquire");
    if (!m_callbacks.is_valid()) {
        return nullptr;
    }

    bool expected = false;
    while (!m_growth_lock.compare_exchange_weak(expected, true,
                                                std::memory_order_acquire,
                                                std::memory_order_relaxed)) {
        expected = false;
        TaggedSlab current = m_head.load(std::memory_order_acquire);
        if (current.ptr != nullptr) {
            return acquire_slab();
        }
        cpu_pause();
    }

    // Double-check head under growth lock
    TaggedSlab current = m_head.load(std::memory_order_acquire);
    if (current.ptr != nullptr) {
        m_growth_lock.store(false, std::memory_order_release);
        return acquire_slab();
    }

    usize count = m_chunk_slabs;
    usize total_bytes = count * SLAB_SIZE;
    void* raw_mem = m_callbacks.alloc(total_bytes, SLAB_SIZE, m_callbacks.user_data);
    if (raw_mem == nullptr) {
        m_growth_lock.store(false, std::memory_order_release);
        return nullptr;
    }

    auto* chunk = new (std::nothrow) Chunk{raw_mem, count, nullptr, true};
    if (chunk == nullptr) {
        m_callbacks.free(raw_mem, total_bytes, SLAB_SIZE, m_callbacks.user_data);
        m_growth_lock.store(false, std::memory_order_release);
        return nullptr;
    }

    // Link chunk into chunk list
    Chunk* old_chunks = m_chunks.load(std::memory_order_relaxed);
    do {
        chunk->next = old_chunks;
    } while (!m_chunks.compare_exchange_weak(old_chunks, chunk,
                                             std::memory_order_release,
                                             std::memory_order_relaxed));

    m_total_slabs.fetch_add(count, std::memory_order_relaxed);
    m_chunk_count.fetch_add(1, std::memory_order_relaxed);

    auto* base_bytes = static_cast<std::byte*>(raw_mem);
    auto* acquired = reinterpret_cast<Slab*>(base_bytes);
    acquired->next.store(nullptr, std::memory_order_relaxed);

    if (count > 1) {
        usize rest_count = count - 1;
        auto* rest_base = base_bytes + SLAB_SIZE;
        for (usize i = 0; i < rest_count; ++i) {
            auto* curr = reinterpret_cast<Slab*>(rest_base + (i * SLAB_SIZE));
            auto* nxt = (i + 1 < rest_count)
                ? reinterpret_cast<Slab*>(rest_base + ((i + 1) * SLAB_SIZE))
                : nullptr;
            curr->next.store(nxt, std::memory_order_relaxed);
        }

        auto* head_rest = reinterpret_cast<Slab*>(rest_base);
        auto* tail_rest = reinterpret_cast<Slab*>(rest_base + ((rest_count - 1) * SLAB_SIZE));
        release_slab_chain(head_rest, tail_rest, rest_count);
    }

    m_growth_lock.store(false, std::memory_order_release);
    return acquired;
}

BlockPool& BlockPool::global() noexcept {
    static BlockPool s_globalPool;
    return s_globalPool;
}

} // namespace lf
