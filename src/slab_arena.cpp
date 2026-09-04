#include <lightflow/core/slab_arena.hpp>

#include <cstdlib>

#if defined(_WIN32)
    #include <malloc.h>
#else
    #include <stdlib.h>
#endif

namespace lf {

// SlabMemoryResource implementation
void* SlabMemoryResource::do_allocate(std::size_t bytes, std::size_t alignment) {
    return m_arena->allocate(bytes, alignment);
}

void SlabMemoryResource::do_deallocate(void* p, std::size_t bytes, std::size_t alignment) noexcept {
    m_arena->deallocate(p, bytes, alignment);
}

bool SlabMemoryResource::do_is_equal(const std::pmr::memory_resource& other) const noexcept {
    return this == &other;
}

// SlabArena implementation
SlabArena::SlabArena(BlockPool& pool) noexcept
    : m_pool(&pool), m_resource(*this) {}

SlabArena::~SlabArena() noexcept {
    reset();
}

SlabArena::SlabArena(SlabArena&& other) noexcept
    : m_pool(other.m_pool),
      m_head_slab(other.m_head_slab),
      m_tail_slab(other.m_tail_slab),
      m_current_slab(other.m_current_slab),
      m_bump_ptr(other.m_bump_ptr),
      m_bump_end(other.m_bump_end),
      m_slab_count(other.m_slab_count),
      m_allocated_bytes(other.m_allocated_bytes),
      m_oversized_head(other.m_oversized_head),
      m_resource(*this) {
    other.m_pool = nullptr;
    other.m_head_slab = nullptr;
    other.m_tail_slab = nullptr;
    other.m_current_slab = nullptr;
    other.m_bump_ptr = nullptr;
    other.m_bump_end = nullptr;
    other.m_slab_count = 0;
    other.m_allocated_bytes = 0;
    other.m_oversized_head = nullptr;
}

SlabArena& SlabArena::operator=(SlabArena&& other) noexcept {
    if (this != &other) {
        reset();

        m_pool = other.m_pool;
        m_head_slab = other.m_head_slab;
        m_tail_slab = other.m_tail_slab;
        m_current_slab = other.m_current_slab;
        m_bump_ptr = other.m_bump_ptr;
        m_bump_end = other.m_bump_end;
        m_slab_count = other.m_slab_count;
        m_allocated_bytes = other.m_allocated_bytes;
        m_oversized_head = other.m_oversized_head;

        other.m_pool = nullptr;
        other.m_head_slab = nullptr;
        other.m_tail_slab = nullptr;
        other.m_current_slab = nullptr;
        other.m_bump_ptr = nullptr;
        other.m_bump_end = nullptr;
        other.m_slab_count = 0;
        other.m_allocated_bytes = 0;
        other.m_oversized_head = nullptr;
    }
    return *this;
}

void* SlabArena::allocate(usize bytes, usize alignment) noexcept {
    LF_ZONE_NAMED(zone, "SlabArena::allocate");
    if (bytes == 0) [[unlikely]] {
        return nullptr;
    }

    LF_ASSERT((alignment & (alignment - 1)) == 0 && alignment > 0);
    if (alignment < alignof(std::max_align_t)) {
        alignment = alignof(std::max_align_t);
    }

    if (m_current_slab != nullptr) {
        uintptr_t curr_addr = reinterpret_cast<uintptr_t>(m_bump_ptr);
        uintptr_t aligned_addr = (curr_addr + (alignment - 1)) & ~(alignment - 1);
        uintptr_t end_addr = reinterpret_cast<uintptr_t>(m_bump_end);

        if (aligned_addr + bytes <= end_addr) {
            m_bump_ptr = reinterpret_cast<std::byte*>(aligned_addr + bytes);
            m_allocated_bytes += bytes;
            return reinterpret_cast<void*>(aligned_addr);
        }
    }

    return allocate_slow(bytes, alignment);
}

void SlabArena::deallocate(void* ptr, usize bytes, usize alignment) noexcept {
    (void)ptr;
    (void)bytes;
    (void)alignment;
}

void* SlabArena::allocate_slow(usize bytes, usize alignment) noexcept {
    LF_ZONE_NAMED(zone, "SlabArena::allocate_slow");
    if (bytes <= Slab::PAYLOAD_SIZE && alignment <= CACHELINE_SIZE) {
        if (m_pool == nullptr) {
            return nullptr;
        }

        Slab* new_slab = m_pool->acquire_slab();
        if (new_slab == nullptr) {
            return nullptr;
        }

        new_slab->next.store(m_head_slab, std::memory_order_relaxed);
        m_head_slab = new_slab;
        if (m_tail_slab == nullptr) {
            m_tail_slab = new_slab;
        }

        m_current_slab = new_slab;
        m_slab_count++;

        std::byte* payload = new_slab->payload();
        m_bump_ptr = payload;
        m_bump_end = reinterpret_cast<std::byte*>(new_slab) + SLAB_SIZE;

        uintptr_t curr_addr = reinterpret_cast<uintptr_t>(m_bump_ptr);
        uintptr_t aligned_addr = (curr_addr + (alignment - 1)) & ~(alignment - 1);
        LF_ASSERT(aligned_addr + bytes <= reinterpret_cast<uintptr_t>(m_bump_end));

        m_bump_ptr = reinterpret_cast<std::byte*>(aligned_addr + bytes);
        m_allocated_bytes += bytes;
        return reinterpret_cast<void*>(aligned_addr);
    }

    // Oversized or large alignment allocation
    usize total_bytes = bytes + sizeof(OversizedBlock) + alignment;
    usize alloc_align = alignment > CACHELINE_SIZE ? alignment : CACHELINE_SIZE;
    void* raw_mem = nullptr;

    if (m_pool != nullptr && m_pool->callbacks().is_valid()) {
        raw_mem = m_pool->callbacks().alloc(total_bytes, alloc_align, m_pool->callbacks().user_data);
    }
#if !defined(LF_DISABLE_PLATFORM_ALLOCATOR)
    else {
#if defined(_WIN32)
        raw_mem = _aligned_malloc(total_bytes, alloc_align);
#else
        int res = ::posix_memalign(&raw_mem, alloc_align, total_bytes);
        if (res != 0) {
            raw_mem = nullptr;
        }
#endif
    }
#endif

    if (raw_mem == nullptr) {
        return nullptr;
    }

    auto* block = reinterpret_cast<OversizedBlock*>(raw_mem);
    block->raw_ptr = raw_mem;
    block->total_bytes = total_bytes;
    block->alignment = alloc_align;
    block->next = m_oversized_head;

    m_oversized_head = block;
    m_allocated_bytes += bytes;

    uintptr_t raw_addr = reinterpret_cast<uintptr_t>(raw_mem) + sizeof(OversizedBlock);
    uintptr_t aligned_addr = (raw_addr + (alignment - 1)) & ~(alignment - 1);
    return reinterpret_cast<void*>(aligned_addr);
}

void SlabArena::reset() noexcept {
    LF_ZONE_NAMED(zone, "SlabArena::reset");
    if (m_head_slab != nullptr && m_pool != nullptr) {
        m_pool->release_slab_chain(m_head_slab, m_tail_slab, m_slab_count);
        m_head_slab = nullptr;
        m_tail_slab = nullptr;
        m_current_slab = nullptr;
    }
    m_bump_ptr = nullptr;
    m_bump_end = nullptr;
    m_slab_count = 0;
    m_allocated_bytes = 0;

    OversizedBlock* curr = m_oversized_head;
    while (curr != nullptr) {
        OversizedBlock* next = curr->next;
        void* raw_ptr = curr->raw_ptr;
        usize total_bytes = curr->total_bytes;
        usize alignment = curr->alignment;

        if (m_pool != nullptr && m_pool->callbacks().is_valid()) {
            m_pool->callbacks().free(raw_ptr, total_bytes, alignment, m_pool->callbacks().user_data);
        }
#if !defined(LF_DISABLE_PLATFORM_ALLOCATOR)
        else {
#if defined(_WIN32)
            _aligned_free(raw_ptr);
#else
            ::free(raw_ptr);
#endif
        }
#endif
        curr = next;
    }
    m_oversized_head = nullptr;
}

} // namespace lf
