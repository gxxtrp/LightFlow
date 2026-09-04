#pragma once

#include <lightflow/core/types.hpp>
#include <lightflow/core/memory_pool.hpp>
#include <lightflow/core/slab_arena.hpp>
#include <lightflow/task/task_domain.hpp>
#include <lightflow/task/task_node.hpp>
#include <lightflow/task/task_handle.hpp>
#include <lightflow/task/parallel_for.hpp>
#include <lightflow/gpu/timeline_sync_point.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>

namespace lf {

class TaskScheduler;
class Subflow;
struct TimelineWaitNode;

/// High-performance C++23 task graph engine.
/// Manages a directed acyclic graph (DAG) of task nodes with lock-free atomic dependency tracking.
/// Features:
/// - Zero steady-state heap allocations: nodes, edges, and callables allocate from an internal SlabArena.
/// - O(1) bulk graph clear and slab reclamation.
/// - Inline Dual-State execution: initialInDegree is preserved, allowing repeat execution
///   of identical graphs with zero allocations and sub-microsecond re-initialization.
class alignas(lf::CACHELINE_SIZE) TaskGraph {
public:
    explicit TaskGraph(BlockPool* pool = nullptr);
    ~TaskGraph() noexcept;

    TaskGraph(const TaskGraph&) = delete;
    TaskGraph& operator=(const TaskGraph&) = delete;
    TaskGraph(TaskGraph&&) = delete;
    TaskGraph& operator=(TaskGraph&&) = delete;

    // --- Task Creation ---

    /// Emplaces a new static task node into the graph.
    template <typename F>
    TaskHandle emplace(F&& callable) {
        return emplace("", TaskDomain::Worker, TaskPriority::Normal, std::forward<F>(callable));
    }

    /// Emplaces a named static task node into the graph.
    template <typename F>
    TaskHandle emplace(const char* name, F&& callable) {
        return emplace(name, TaskDomain::Worker, TaskPriority::Normal, std::forward<F>(callable));
    }

    /// Emplaces a named static task node with a specific execution domain.
    template <typename F>
    TaskHandle emplace(const char* name, TaskDomain domain, F&& callable) {
        return emplace(name, domain, TaskPriority::Normal, std::forward<F>(callable));
    }

    /// Emplaces a named static task node with domain and priority.
    template <typename F>
    TaskHandle emplace(const char* name, TaskDomain domain, TaskPriority priority, F&& callable) {
        TaskNode* node = m_arena.create<TaskNode>();
        node->id = m_nodeCount++;
        node->name = name;
        node->domain = domain;
        node->priority = priority;
        node->graph = this;
        node->setCallable(std::forward<F>(callable), &m_arena);

        appendNode(node);
        return TaskHandle{node};
    }

    // --- Conditional Branching ---

    /// Emplaces a condition task node returning an integer branch index.
    template <typename F>
    ConditionHandle emplaceCondition(F&& callable) {
        return emplaceCondition("", TaskDomain::Worker, TaskPriority::Normal, std::forward<F>(callable));
    }

    /// Emplaces a named condition task node.
    template <typename F>
    ConditionHandle emplaceCondition(const char* name, F&& callable) {
        return emplaceCondition(name, TaskDomain::Worker, TaskPriority::Normal, std::forward<F>(callable));
    }

    /// Emplaces a named condition task node with a specific domain.
    template <typename F>
    ConditionHandle emplaceCondition(const char* name, TaskDomain domain, F&& callable) {
        return emplaceCondition(name, domain, TaskPriority::Normal, std::forward<F>(callable));
    }

    /// Emplaces a named condition task node with domain and priority.
    template <typename F>
    ConditionHandle emplaceCondition(const char* name, TaskDomain domain, TaskPriority priority, F&& callable) {
        TaskNode* node = m_arena.create<TaskNode>();
        node->id = m_nodeCount++;
        node->name = name;
        node->domain = domain;
        node->priority = priority;
        node->graph = this;
        node->setConditionCallable(std::forward<F>(callable), &m_arena);

        appendNode(node);
        return ConditionHandle{node};
    }

    // --- Dynamic Subflows ---

    /// Emplaces a dynamic subflow task node.
    template <typename F>
    TaskHandle emplaceSubflow(F&& callable) {
        return emplaceSubflow("", TaskDomain::Worker, TaskPriority::Normal, std::forward<F>(callable));
    }

    /// Emplaces a named dynamic subflow task node.
    template <typename F>
    TaskHandle emplaceSubflow(const char* name, F&& callable) {
        return emplaceSubflow(name, TaskDomain::Worker, TaskPriority::Normal, std::forward<F>(callable));
    }

    /// Emplaces a named dynamic subflow task node with domain.
    template <typename F>
    TaskHandle emplaceSubflow(const char* name, TaskDomain domain, F&& callable) {
        return emplaceSubflow(name, domain, TaskPriority::Normal, std::forward<F>(callable));
    }

    /// Emplaces a named dynamic subflow task node with domain and priority.
    template <typename F>
    TaskHandle emplaceSubflow(const char* name, TaskDomain domain, TaskPriority priority, F&& callable);

    // --- Data-Parallel Loops (parallelFor) ---

    /// Partitions an iteration range [0, count) into chunks of batchSize elements,
    /// executing them concurrently across worker threads with an integrated stackless barrier.
    template <typename F>
    ParallelForHandle parallelFor(
        const char* name,
        usize count,
        usize batchSize,
        F&& callable,
        TaskDomain domain = TaskDomain::Worker,
        TaskPriority priority = TaskPriority::Normal
    ) {
        batchSize = (batchSize == 0) ? 1 : batchSize;

        if (count == 0) {
            TaskNode* passThrough = m_arena.create<TaskNode>();
            passThrough->id = m_nodeCount++;
            passThrough->name = name;
            passThrough->domain = domain;
            passThrough->priority = priority;
            passThrough->graph = this;
            passThrough->type = TaskType::ParallelForChunk;
            passThrough->setCallable([]() noexcept {}, &m_arena);
            appendNode(passThrough);
            return ParallelForHandle{passThrough, passThrough, 0, {}};
        }

        const usize numChunks = (count + batchSize - 1) / batchSize;

        if (numChunks == 1) {
            TaskNode* chunkNode = m_arena.create<TaskNode>();
            chunkNode->id = m_nodeCount++;
            chunkNode->name = name;
            chunkNode->domain = domain;
            chunkNode->priority = priority;
            chunkNode->graph = this;
            chunkNode->type = TaskType::ParallelForChunk;

            ChunkRange range{0, count, 0, 1};
            using DecayedF = std::decay_t<F>;

            if constexpr (ChunkRangeInvocable<DecayedF>) {
                chunkNode->setCallable([f = std::forward<F>(callable), range]() mutable noexcept {
                    f(range);
                }, &m_arena);
            } else if constexpr (RangeInvocableWithChunkIdx<DecayedF>) {
                chunkNode->setCallable([f = std::forward<F>(callable), start = range.start, end = range.end]() mutable noexcept {
                    f(start, end, 0);
                }, &m_arena);
            } else if constexpr (RangeInvocable<DecayedF>) {
                chunkNode->setCallable([f = std::forward<F>(callable), start = range.start, end = range.end]() mutable noexcept {
                    f(start, end);
                }, &m_arena);
            } else if constexpr (ChunkIndexInvocable<DecayedF>) {
                chunkNode->setCallable([f = std::forward<F>(callable)]() mutable noexcept {
                    f(ChunkIndex{0});
                }, &m_arena);
            } else {
                chunkNode->setCallable([f = std::forward<F>(callable), start = range.start, end = range.end]() mutable noexcept {
                    for (usize i = start; i < end; ++i) {
                        f(i);
                    }
                }, &m_arena);
            }

            appendNode(chunkNode);
            std::span<TaskNode*> chunks = m_arena.allocate_span<TaskNode*>(1);
            chunks[0] = chunkNode;
            return ParallelForHandle{chunkNode, chunkNode, 1, chunks};
        }

        using DecayedF = std::decay_t<F>;
        DecayedF* sharedCallable = m_arena.create<DecayedF>(std::forward<F>(callable));

        TaskNode* entryNode = m_arena.create<TaskNode>();
        entryNode->id = m_nodeCount++;
        entryNode->name = name;
        entryNode->domain = domain;
        entryNode->priority = priority;
        entryNode->graph = this;
        entryNode->type = TaskType::Static;
        entryNode->setCallable([]() noexcept {}, &m_arena);
        appendNode(entryNode);

        TaskNode* joinBarrierNode = m_arena.create<TaskNode>();
        joinBarrierNode->id = m_nodeCount++;
        joinBarrierNode->name = name;
        joinBarrierNode->domain = domain;
        joinBarrierNode->priority = priority;
        joinBarrierNode->graph = this;
        joinBarrierNode->type = TaskType::Static;
        joinBarrierNode->setCallable([]() noexcept {}, &m_arena);
        joinBarrierNode->userData = sharedCallable;

        if constexpr (!std::is_trivially_destructible_v<DecayedF>) {
            joinBarrierNode->destroyFn = [](TaskNode* node) noexcept {
                auto* ptr = static_cast<DecayedF*>(node->userData);
                if (ptr != nullptr) {
                    ptr->~DecayedF();
                    node->userData = nullptr;
                }
            };
        }

        std::span<TaskNode*> chunks = m_arena.allocate_span<TaskNode*>(numChunks);

        for (usize c = 0; c < numChunks; ++c) {
            const usize start = c * batchSize;
            const usize end = (std::min)(count, start + batchSize);
            const ChunkRange range{start, end, c, numChunks};

            TaskNode* chunkNode = m_arena.create<TaskNode>();
            chunkNode->id = m_nodeCount++;
            chunkNode->name = name;
            chunkNode->domain = domain;
            chunkNode->priority = priority;
            chunkNode->graph = this;
            chunkNode->type = TaskType::ParallelForChunk;

            if constexpr (ChunkRangeInvocable<DecayedF>) {
                chunkNode->setCallable([sharedCallable, range]() noexcept {
                    (*sharedCallable)(range);
                }, &m_arena);
            } else if constexpr (RangeInvocableWithChunkIdx<DecayedF>) {
                chunkNode->setCallable([sharedCallable, start, end, c]() noexcept {
                    (*sharedCallable)(start, end, c);
                }, &m_arena);
            } else if constexpr (RangeInvocable<DecayedF>) {
                chunkNode->setCallable([sharedCallable, start, end]() noexcept {
                    (*sharedCallable)(start, end);
                }, &m_arena);
            } else if constexpr (ChunkIndexInvocable<DecayedF>) {
                chunkNode->setCallable([sharedCallable, c]() noexcept {
                    (*sharedCallable)(ChunkIndex{c});
                }, &m_arena);
            } else {
                chunkNode->setCallable([sharedCallable, start, end]() noexcept {
                    for (usize i = start; i < end; ++i) {
                        (*sharedCallable)(i);
                    }
                }, &m_arena);
            }

            appendNode(chunkNode);
            chunks[c] = chunkNode;

            addEdge(entryNode, chunkNode);
            addEdge(chunkNode, joinBarrierNode);
        }

        appendNode(joinBarrierNode);
        return ParallelForHandle{entryNode, joinBarrierNode, numChunks, chunks};
    }

    /// Partitions an iteration range without an explicit task name.
    template <typename F>
    ParallelForHandle parallelFor(
        usize count,
        usize batchSize,
        F&& callable,
        TaskDomain domain = TaskDomain::Worker,
        TaskPriority priority = TaskPriority::Normal
    ) {
        return parallelFor("", count, batchSize, std::forward<F>(callable), domain, priority);
    }

    /// Partitions iteration range dispatching each chunk by its chunk index [0, numChunks).
    template <typename F>
    ParallelForHandle parallelFor(
        const char* name,
        usize count,
        usize batchSize,
        ByChunkTag,
        F&& callable,
        TaskDomain domain = TaskDomain::Worker,
        TaskPriority priority = TaskPriority::Normal
    ) {
        batchSize = (batchSize == 0) ? 1 : batchSize;

        if (count == 0) {
            TaskNode* passThrough = m_arena.create<TaskNode>();
            passThrough->id = m_nodeCount++;
            passThrough->name = name;
            passThrough->domain = domain;
            passThrough->priority = priority;
            passThrough->graph = this;
            passThrough->type = TaskType::ParallelForChunk;
            passThrough->setCallable([]() noexcept {}, &m_arena);
            appendNode(passThrough);
            return ParallelForHandle{passThrough, passThrough, 0, {}};
        }

        const usize numChunks = (count + batchSize - 1) / batchSize;

        if (numChunks == 1) {
            TaskNode* chunkNode = m_arena.create<TaskNode>();
            chunkNode->id = m_nodeCount++;
            chunkNode->name = name;
            chunkNode->domain = domain;
            chunkNode->priority = priority;
            chunkNode->graph = this;
            chunkNode->type = TaskType::ParallelForChunk;

            chunkNode->setCallable([f = std::forward<F>(callable)]() mutable noexcept {
                f(usize{0});
            }, &m_arena);

            appendNode(chunkNode);
            std::span<TaskNode*> chunks = m_arena.allocate_span<TaskNode*>(1);
            chunks[0] = chunkNode;
            return ParallelForHandle{chunkNode, chunkNode, 1, chunks};
        }

        using DecayedF = std::decay_t<F>;
        DecayedF* sharedCallable = m_arena.create<DecayedF>(std::forward<F>(callable));

        TaskNode* entryNode = m_arena.create<TaskNode>();
        entryNode->id = m_nodeCount++;
        entryNode->name = name;
        entryNode->domain = domain;
        entryNode->priority = priority;
        entryNode->graph = this;
        entryNode->type = TaskType::Static;
        entryNode->setCallable([]() noexcept {}, &m_arena);
        appendNode(entryNode);

        TaskNode* joinBarrierNode = m_arena.create<TaskNode>();
        joinBarrierNode->id = m_nodeCount++;
        joinBarrierNode->name = name;
        joinBarrierNode->domain = domain;
        joinBarrierNode->priority = priority;
        joinBarrierNode->graph = this;
        joinBarrierNode->type = TaskType::Static;
        joinBarrierNode->setCallable([]() noexcept {}, &m_arena);
        joinBarrierNode->userData = sharedCallable;

        if constexpr (!std::is_trivially_destructible_v<DecayedF>) {
            joinBarrierNode->destroyFn = [](TaskNode* node) noexcept {
                auto* ptr = static_cast<DecayedF*>(node->userData);
                if (ptr != nullptr) {
                    ptr->~DecayedF();
                    node->userData = nullptr;
                }
            };
        }

        std::span<TaskNode*> chunks = m_arena.allocate_span<TaskNode*>(numChunks);

        for (usize c = 0; c < numChunks; ++c) {
            TaskNode* chunkNode = m_arena.create<TaskNode>();
            chunkNode->id = m_nodeCount++;
            chunkNode->name = name;
            chunkNode->domain = domain;
            chunkNode->priority = priority;
            chunkNode->graph = this;
            chunkNode->type = TaskType::ParallelForChunk;

            chunkNode->setCallable([sharedCallable, c]() noexcept {
                (*sharedCallable)(c);
            }, &m_arena);

            appendNode(chunkNode);
            chunks[c] = chunkNode;

            addEdge(entryNode, chunkNode);
            addEdge(chunkNode, joinBarrierNode);
        }

        appendNode(joinBarrierNode);
        return ParallelForHandle{entryNode, joinBarrierNode, numChunks, chunks};
    }

    /// Partitions an iteration range invoking callable with chunk index.
    template <typename F>
    ParallelForHandle parallelForChunks(
        const char* name,
        usize count,
        usize batchSize,
        F&& callable,
        TaskDomain domain = TaskDomain::Worker,
        TaskPriority priority = TaskPriority::Normal
    ) {
        return parallelFor(name, count, batchSize, ByChunk, std::forward<F>(callable), domain, priority);
    }

    template <typename F>
    ParallelForHandle parallelForChunks(
        usize count,
        usize batchSize,
        F&& callable,
        TaskDomain domain = TaskDomain::Worker,
        TaskPriority priority = TaskPriority::Normal
    ) {
        return parallelFor("", count, batchSize, ByChunk, std::forward<F>(callable), domain, priority);
    }

    /// Partitions a std::span<T> into chunks of batchSize elements.
    template <typename T, typename F>
    ParallelForHandle parallelFor(
        const char* name,
        std::span<T> slice,
        usize batchSize,
        F&& callable,
        TaskDomain domain = TaskDomain::Worker,
        TaskPriority priority = TaskPriority::Normal
    ) {
        using DecayedF = std::decay_t<F>;
        if constexpr (SpanChunkInvocable<DecayedF, T>) {
            return parallelFor(name, slice.size(), batchSize, [slice, f = std::forward<F>(callable)](usize start, usize end) mutable noexcept {
                f(slice.subspan(start, end - start));
            }, domain, priority);
        } else if constexpr (SpanItemWithIndexInvocable<DecayedF, T>) {
            return parallelFor(name, slice.size(), batchSize, [slice, f = std::forward<F>(callable)](usize start, usize end) mutable noexcept {
                for (usize i = start; i < end; ++i) {
                    f(slice[i], i);
                }
            }, domain, priority);
        } else if constexpr (SpanItemInvocable<DecayedF, T>) {
            return parallelFor(name, slice.size(), batchSize, [slice, f = std::forward<F>(callable)](usize start, usize end) mutable noexcept {
                for (usize i = start; i < end; ++i) {
                    f(slice[i]);
                }
            }, domain, priority);
        } else {
            return parallelFor(name, slice.size(), batchSize, std::forward<F>(callable), domain, priority);
        }
    }

    template <typename T, typename F>
    ParallelForHandle parallelFor(
        std::span<T> slice,
        usize batchSize,
        F&& callable,
        TaskDomain domain = TaskDomain::Worker,
        TaskPriority priority = TaskPriority::Normal
    ) {
        return parallelFor("", slice, batchSize, std::forward<F>(callable), domain, priority);
    }

    // --- Edge Construction ---

    /// Adds a directed dependency edge: from -> to.
    /// Allocates the SuccessorEdge from the graph's internal SlabArena.
    void addEdge(TaskNode* from, TaskNode* to, int branch = -1) noexcept;

    /// Registers a GPU timeline synchronization wait barrier for the specified task node.
    /// Allocates the TimelineWaitNode from the graph's internal SlabArena.
    void addWaitPoint(TaskNode* node, const TimelineSyncPoint& syncPoint) noexcept;

    /// Returns the head of the intrusive list of timeline wait nodes registered in this graph.
    LF_NODISCARD TimelineWaitNode* firstWaitNode() const noexcept { return m_firstWaitNode; }

    // --- Execution Lifecycle ---

    /// Prepares the graph for execution and dispatches root tasks into the scheduler.
    /// Resets each node's inDegree to initialInDegree in O(N) with zero allocations.
    void prepareRun(TaskScheduler& scheduler) noexcept;

    /// Returns true if all tasks in the graph have completed execution.
    LF_NODISCARD bool isCompleted() const noexcept {
        return m_pendingTasks.load(std::memory_order_acquire) == 0;
    }

    /// Resets the graph, destroying any active callables and reclaiming all slab memory in O(1).
    void clear() noexcept;

    // --- Metrics & Accessors ---

    LF_NODISCARD u32 nodeCount() const noexcept { return m_nodeCount; }
    LF_NODISCARD u32 pendingTaskCount() const noexcept {
        return m_pendingTasks.load(std::memory_order_relaxed);
    }
    LF_NODISCARD bool empty() const noexcept { return m_nodeCount == 0; }
    LF_NODISCARD SlabArena& arena() noexcept { return m_arena; }
    LF_NODISCARD TaskScheduler* scheduler() const noexcept { return m_scheduler; }

    /// Internal scheduler notification entry points:
    void scheduleNode(TaskNode* node, bool notify = true) noexcept;
    void onTaskCompleted(TaskNode* node) noexcept;
    void addPendingTasks(u32 count) noexcept {
        m_pendingTasks.fetch_add(count, std::memory_order_release);
    }

private:
    friend class Subflow;
    friend struct TaskNode;

    void appendNode(TaskNode* node) noexcept;

    SlabArena m_arena;
    TaskNode* m_firstNode{nullptr};
    TaskNode* m_lastNode{nullptr};
    TimelineWaitNode* m_firstWaitNode{nullptr};
    u32 m_nodeCount{0};

    LF_ALIGN_CACHELINE std::atomic<u32> m_pendingTasks{0};
    TaskScheduler* m_scheduler{nullptr};
};

inline TaskHandle TaskHandle::precede(TaskHandle other) const noexcept {
    if (m_exit != nullptr && other.m_entry != nullptr) {
        TaskGraph* g = m_exit->graph != nullptr ? m_exit->graph : other.m_entry->graph;
        if (g != nullptr) {
            g->addEdge(m_exit, other.m_entry);
        }
    }
    return *this;
}

inline TaskHandle BranchHandle::precede(TaskHandle other) const noexcept {
    if (m_node != nullptr && other.entryNode() != nullptr) {
        TaskGraph* g = m_node->graph != nullptr ? m_node->graph : other.entryNode()->graph;
        if (g != nullptr) {
            g->addEdge(m_node, other.entryNode(), m_branch);
        }
    }
    return other;
}

} // namespace lf

#include <lightflow/task/subflow.hpp>
