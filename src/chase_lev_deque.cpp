#include <lightflow/scheduler/chase_lev_deque.hpp>
#include <lightflow/core/tracy.hpp>

#include <algorithm>
#include <bit>
#include <new>

namespace lf {

class alignas(lf::CACHELINE_SIZE) ChaseLevDeque::RingBuffer {
public:
    explicit RingBuffer(usize capacity)
        : m_capacity(capacity)
        , m_mask(capacity - 1)
        , m_tasks(new (std::nothrow) std::atomic<TaskNode*>[capacity])
    {
        LF_ASSERT(m_tasks != nullptr);
        for (usize i = 0; i < capacity; ++i) {
            m_tasks[i].store(nullptr, std::memory_order_relaxed);
        }
    }

    ~RingBuffer() {
        delete[] m_tasks;
    }

    LF_NODISCARD usize capacity() const noexcept {
        return m_capacity;
    }

    void store(u32 index, TaskNode* task, std::memory_order order = std::memory_order_relaxed) noexcept {
        m_tasks[static_cast<usize>(index) & m_mask].store(task, order);
    }

    LF_NODISCARD TaskNode* load(u32 index, std::memory_order order = std::memory_order_relaxed) const noexcept {
        return m_tasks[static_cast<usize>(index) & m_mask].load(order);
    }

    LF_NODISCARD RingBuffer* grow(u32 top, u32 bottom) noexcept {
        auto* newBuf = new (std::nothrow) RingBuffer(m_capacity * 2);
        LF_ASSERT(newBuf != nullptr);
        if (bottom > top) {
            for (u32 i = top; i != bottom; ++i) {
                newBuf->store(i, load(i, std::memory_order_relaxed), std::memory_order_relaxed);
            }
        }
        return newBuf;
    }

    RingBuffer* nextGarbage{nullptr};

private:
    usize m_capacity;
    usize m_mask;
    std::atomic<TaskNode*>* m_tasks{nullptr};
};

ChaseLevDeque::ChaseLevDeque(usize initialCapacity) {
    usize cap = std::bit_ceil(std::max<usize>(initialCapacity, 16));
    m_top.store(0, std::memory_order_relaxed);
    m_bottom.store(0, std::memory_order_relaxed);
    auto* buf = new (std::nothrow) RingBuffer(cap);
    LF_ASSERT(buf != nullptr);
    m_buffer.store(buf, std::memory_order_relaxed);
}

ChaseLevDeque::~ChaseLevDeque() {
    auto* buf = m_buffer.load(std::memory_order_relaxed);
    delete buf;

    auto* garbage = m_garbageHead;
    while (garbage != nullptr) {
        auto* next = garbage->nextGarbage;
        delete garbage;
        garbage = next;
    }
}

ChaseLevDeque::RingBuffer* ChaseLevDeque::grow(u32 bottom, u32 top) noexcept {
    LF_ZONE_NAMED(zone, "ChaseLevDeque::grow");
    if (bottom <= top) {
        return m_buffer.load(std::memory_order_relaxed);
    }
    auto* curBuf = m_buffer.load(std::memory_order_relaxed);
    auto* newBuf = curBuf->grow(top, bottom);
    curBuf->nextGarbage = m_garbageHead;
    m_garbageHead = curBuf;
    m_buffer.store(newBuf, std::memory_order_release);
    return newBuf;
}

void ChaseLevDeque::pushBottom(TaskNode* task) noexcept {
    u32 b = m_bottom.load(std::memory_order_relaxed);
    u32 t = m_top.load(std::memory_order_acquire);
    auto* buf = m_buffer.load(std::memory_order_relaxed);

    if (b < t) {
        b = t;
        m_bottom.store(b, std::memory_order_relaxed);
    }

    if (b - t >= static_cast<u32>(buf->capacity())) {
        buf = grow(b, t);
    }

    buf->store(b, task, std::memory_order_relaxed);
    m_bottom.store(b + 1, std::memory_order_release);
}

TaskNode* ChaseLevDeque::popBottom() noexcept {
    u32 b = m_bottom.load(std::memory_order_relaxed);
    auto* buf = m_buffer.load(std::memory_order_relaxed);

    u32 t = m_top.load(std::memory_order_relaxed);
    if (b <= t) {
        return nullptr;
    }

    b = b - 1;
    m_bottom.store(b, std::memory_order_seq_cst);

    t = m_top.load(std::memory_order_seq_cst);
    if (t <= b) {
        TaskNode* task = buf->load(b, std::memory_order_relaxed);
        if (t == b) {
            // Compete with concurrent thieves for the last remaining element
            if (!m_top.compare_exchange_strong(
                t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed))
            {
                task = nullptr;
            }
            m_bottom.store(b + 1, std::memory_order_relaxed);
        }
        return task;
    } else {
        // Deque was emptied by a thief
        m_bottom.store(b + 1, std::memory_order_relaxed);
        return nullptr;
    }
}

TaskNode* ChaseLevDeque::stealTop() noexcept {
    u32 t = m_top.load(std::memory_order_acquire);
    u32 b = m_bottom.load(std::memory_order_acquire);
    if (t >= b) {
        return nullptr;
    }

    auto* buf = m_buffer.load(std::memory_order_acquire);
    TaskNode* task = buf->load(t, std::memory_order_relaxed);

    if (m_top.compare_exchange_strong(
        t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed))
    {
        return task;
    }
    return nullptr;
}

usize ChaseLevDeque::stealBatch(std::span<TaskNode*> destination) noexcept {
    if (destination.empty()) {
        return 0;
    }

    u32 t = m_top.load(std::memory_order_acquire);
    u32 b = m_bottom.load(std::memory_order_acquire);
    if (t >= b) {
        return 0;
    }

    u32 available = b - t;
    u32 stealCount = (available + 1) / 2; // ceil(K / 2)
    if (stealCount > static_cast<u32>(destination.size())) {
        stealCount = static_cast<u32>(destination.size());
    }

    usize stolen = 0;
    for (u32 i = 0; i < stealCount; ++i) {
        TaskNode* task = stealTop();
        if (task == nullptr) {
            break;
        }
        destination[stolen++] = task;
    }
    return stolen;
}

usize ChaseLevDeque::stealBatch(ChaseLevDeque& destination) noexcept {
    TaskNode* batch[64];
    usize stolen = stealBatch(std::span<TaskNode*>(batch, 64));
    for (usize i = 0; i < stolen; ++i) {
        destination.pushBottom(batch[i]);
    }
    return stolen;
}

usize ChaseLevDeque::size() const noexcept {
    u32 t = m_top.load(std::memory_order_relaxed);
    u32 b = m_bottom.load(std::memory_order_relaxed);
    return (b > t) ? static_cast<usize>(b - t) : 0;
}

bool ChaseLevDeque::empty() const noexcept {
    u32 t = m_top.load(std::memory_order_relaxed);
    u32 b = m_bottom.load(std::memory_order_relaxed);
    return b <= t;
}

usize ChaseLevDeque::capacity() const noexcept {
    auto* buf = m_buffer.load(std::memory_order_relaxed);
    return buf != nullptr ? buf->capacity() : 0;
}

DualPriorityQueue::DualPriorityQueue(usize initialCapacity)
    : m_high(initialCapacity)
    , m_normal(initialCapacity)
{}

void DualPriorityQueue::push(TaskNode* task, TaskPriority priority) noexcept {
    if (priority == TaskPriority::High) {
        m_high.pushBottom(task);
    } else {
        m_normal.pushBottom(task);
    }
}

TaskNode* DualPriorityQueue::pop() noexcept {
    TaskNode* task = m_high.popBottom();
    if (task != nullptr) {
        return task;
    }
    return m_normal.popBottom();
}

TaskNode* DualPriorityQueue::steal() noexcept {
    TaskNode* task = m_high.stealTop();
    if (task != nullptr) {
        return task;
    }
    return m_normal.stealTop();
}

usize DualPriorityQueue::stealBatch(std::span<TaskNode*> destination) noexcept {
    usize stolen = m_high.stealBatch(destination);
    if (stolen > 0) {
        return stolen;
    }
    return m_normal.stealBatch(destination);
}

usize DualPriorityQueue::stealBatch(DualPriorityQueue& destination) noexcept {
    usize stolen = m_high.stealBatch(destination.m_high);
    if (stolen > 0) {
        return stolen;
    }
    return m_normal.stealBatch(destination.m_normal);
}

bool DualPriorityQueue::empty() const noexcept {
    return m_high.empty() && m_normal.empty();
}

usize DualPriorityQueue::size() const noexcept {
    return m_high.size() + m_normal.size();
}

ChaseLevDeque& DualPriorityQueue::deque(TaskPriority priority) noexcept {
    return (priority == TaskPriority::High) ? m_high : m_normal;
}

const ChaseLevDeque& DualPriorityQueue::deque(TaskPriority priority) const noexcept {
    return (priority == TaskPriority::High) ? m_high : m_normal;
}

} // namespace lf
