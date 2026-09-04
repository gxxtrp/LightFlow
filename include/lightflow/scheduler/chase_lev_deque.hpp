#pragma once

#include <lightflow/core/types.hpp>

#include <atomic>
#include <span>
#include <cstddef>
#include <cstdint>

#include <lightflow/task/task_domain.hpp>
#include <lightflow/task/task_node.hpp>

namespace lf {

/// Lock-free, work-stealing circular ring buffer based on the Chase-Lev algorithm.
/// Optimized for C++23 atomic acquire-release semantics, cacheline isolation,
/// dynamic power-of-2 growth, and batch work-stealing.
class alignas(lf::CACHELINE_SIZE) ChaseLevDeque {
public:
    /// Constructs a Chase-Lev deque with the specified initial capacity.
    /// The capacity is rounded up to the nearest power of 2 (minimum 16).
    explicit ChaseLevDeque(usize initialCapacity = 1024);

    /// Destructor. Reclaims all active and retired ring buffers.
    ~ChaseLevDeque();

    ChaseLevDeque(const ChaseLevDeque&) = delete;
    ChaseLevDeque& operator=(const ChaseLevDeque&) = delete;
    ChaseLevDeque(ChaseLevDeque&&) = delete;
    ChaseLevDeque& operator=(ChaseLevDeque&&) = delete;

    // --- Worker Interface (Single Producer, LIFO) ---

    /// Pushes a task onto the bottom of the deque.
    /// May trigger dynamic buffer growth if full.
    void pushBottom(TaskNode* task) noexcept;

    /// Pops a task from the bottom of the deque in LIFO order.
    /// Returns nullptr if the deque is empty or if the last element was stolen.
    LF_NODISCARD TaskNode* popBottom() noexcept;

    /// Returns the approximate number of tasks currently in the deque.
    LF_NODISCARD usize size() const noexcept;

    /// Returns true if the deque is empty.
    LF_NODISCARD bool empty() const noexcept;

    /// Returns the current ring buffer capacity.
    LF_NODISCARD usize capacity() const noexcept;

    // --- Thief Interface (Multi-Consumer, FIFO) ---

    /// Steals a single task from the top of the deque in FIFO order.
    /// Returns nullptr if the deque is empty or if the CAS race was lost.
    LF_NODISCARD TaskNode* stealTop() noexcept;

    /// Batch work-stealing: steals up to ceil(K / 2) ready tasks in a single atomic CAS.
    /// Stolen tasks are copied into the destination span in FIFO order.
    /// Returns the actual number of tasks stolen (0 if empty or CAS failed).
    LF_NODISCARD usize stealBatch(std::span<TaskNode*> destination) noexcept;

    /// Batch work-stealing: steals up to ceil(K / 2) tasks and pushes them directly
    /// into the caller's (thief's) deque.
    LF_NODISCARD usize stealBatch(ChaseLevDeque& destination) noexcept;

private:
    class RingBuffer;

    RingBuffer* grow(u32 bottom, u32 top) noexcept;

    // Top index (strictly monotonic, accessed and CASed by thieves).
    LF_ALIGN_CACHELINE std::atomic<u32> m_top{0};

    // Bottom index (accessed and modified solely by the worker).
    LF_ALIGN_CACHELINE std::atomic<u32> m_bottom{0};

    // Active circular buffer pointer.
    LF_ALIGN_CACHELINE std::atomic<RingBuffer*> m_buffer{nullptr};

    // Intrusive singly-linked list of retired buffers for safe reclamation at shutdown.
    // Accessed and modified solely by the worker thread during growth.
    RingBuffer* m_garbageHead{nullptr};
};

/// Dual-priority coordination queue wrapping High and Normal Chase-Lev deques.
/// Strictly drains High priority tasks before Normal priority tasks for both
/// worker execution (pop) and thief stealing (steal / stealBatch).
class alignas(lf::CACHELINE_SIZE) DualPriorityQueue {
public:
    explicit DualPriorityQueue(usize initialCapacity = 1024);
    ~DualPriorityQueue() = default;

    DualPriorityQueue(const DualPriorityQueue&) = delete;
    DualPriorityQueue& operator=(const DualPriorityQueue&) = delete;
    DualPriorityQueue(DualPriorityQueue&&) = delete;
    DualPriorityQueue& operator=(DualPriorityQueue&&) = delete;

    /// Pushes a task to the queue corresponding to its priority.
    void push(TaskNode* task, TaskPriority priority = TaskPriority::Normal) noexcept;

    /// Worker pops from High deque first; falls back to Normal deque.
    LF_NODISCARD TaskNode* pop() noexcept;

    /// Thief steals from High deque first; falls back to Normal deque.
    LF_NODISCARD TaskNode* steal() noexcept;

    /// Thief batch-steals from High deque first; falls back to Normal deque.
    LF_NODISCARD usize stealBatch(std::span<TaskNode*> destination) noexcept;

    /// Thief batch-steals from victim High/Normal into destination High/Normal.
    LF_NODISCARD usize stealBatch(DualPriorityQueue& destination) noexcept;

    /// Returns true if both High and Normal queues are empty.
    LF_NODISCARD bool empty() const noexcept;

    /// Returns total approximate count of tasks across both queues.
    LF_NODISCARD usize size() const noexcept;

    /// Direct access to underlying single-priority deques.
    LF_NODISCARD ChaseLevDeque& deque(TaskPriority priority) noexcept;
    LF_NODISCARD const ChaseLevDeque& deque(TaskPriority priority) const noexcept;

    LF_NODISCARD ChaseLevDeque& highDeque() noexcept { return m_high; }
    LF_NODISCARD const ChaseLevDeque& highDeque() const noexcept { return m_high; }

    LF_NODISCARD ChaseLevDeque& normalDeque() noexcept { return m_normal; }
    LF_NODISCARD const ChaseLevDeque& normalDeque() const noexcept { return m_normal; }

private:
    ChaseLevDeque m_high;
    ChaseLevDeque m_normal;
};

} // namespace lf
