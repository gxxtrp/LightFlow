#pragma once

#include <lightflow/core/types.hpp>
#include <lightflow/core/memory_pool.hpp>
#include <lightflow/core/slab_arena.hpp>
#include <lightflow/task/task_domain.hpp>
#include <lightflow/task/task_node.hpp>
#include <lightflow/task/task_handle.hpp>
#include <lightflow/task/parallel_for.hpp>
#include <lightflow/scheduler/task_scheduler.hpp>

#include <atomic>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>

namespace lf {

/// Dynamic child graph builder instantiated when a subflow task executes on a worker thread.
/// Allows dynamic construction of child DAGs with zero steady-state heap allocations,
/// wait-free node/edge allocation from SlabArena, and lock-free successor splicing.
class Subflow {
public:
    explicit Subflow(TaskNode* parentNode, SlabArena* arena = nullptr) noexcept
        : m_parentNode(parentNode),
          m_graph(parentNode != nullptr ? parentNode->graph : nullptr),
          m_arena(arena != nullptr ? arena : (m_graph != nullptr ? &m_graph->arena() : nullptr))
    {
    }

    ~Subflow() noexcept = default;

    Subflow(const Subflow&) = delete;
    Subflow& operator=(const Subflow&) = delete;
    Subflow(Subflow&&) = delete;
    Subflow& operator=(Subflow&&) = delete;

    // --- Task Creation ---

    template <typename F>
    TaskHandle emplace(F&& callable) {
        return emplace("", TaskDomain::Worker, TaskPriority::Normal, std::forward<F>(callable));
    }

    template <typename F>
    TaskHandle emplace(const char* name, F&& callable) {
        return emplace(name, TaskDomain::Worker, TaskPriority::Normal, std::forward<F>(callable));
    }

    template <typename F>
    TaskHandle emplace(const char* name, TaskDomain domain, F&& callable) {
        return emplace(name, domain, TaskPriority::Normal, std::forward<F>(callable));
    }

    template <typename F>
    TaskHandle emplace(const char* name, TaskDomain domain, TaskPriority priority, F&& callable) {
        LF_ASSERT(m_arena != nullptr);
        TaskNode* node = m_arena->create<TaskNode>();
        node->id = m_nodeCount++;
        node->name = name;
        node->domain = domain;
        node->priority = priority;
        node->graph = m_graph;
        node->setCallable(std::forward<F>(callable), m_arena);

        appendNode(node);
        return TaskHandle{node};
    }

    // --- Condition Creation ---

    template <typename F>
    ConditionHandle emplaceCondition(F&& callable) {
        return emplaceCondition("", TaskDomain::Worker, TaskPriority::Normal, std::forward<F>(callable));
    }

    template <typename F>
    ConditionHandle emplaceCondition(const char* name, F&& callable) {
        return emplaceCondition(name, TaskDomain::Worker, TaskPriority::Normal, std::forward<F>(callable));
    }

    template <typename F>
    ConditionHandle emplaceCondition(const char* name, TaskDomain domain, F&& callable) {
        return emplaceCondition(name, domain, TaskPriority::Normal, std::forward<F>(callable));
    }

    template <typename F>
    ConditionHandle emplaceCondition(const char* name, TaskDomain domain, TaskPriority priority, F&& callable) {
        LF_ASSERT(m_arena != nullptr);
        TaskNode* node = m_arena->create<TaskNode>();
        node->id = m_nodeCount++;
        node->name = name;
        node->domain = domain;
        node->priority = priority;
        node->graph = m_graph;
        node->setConditionCallable(std::forward<F>(callable), m_arena);

        appendNode(node);
        return ConditionHandle{node};
    }

    // --- Nested Subflows ---

    template <typename F>
    TaskHandle emplaceSubflow(F&& callable) {
        return emplaceSubflow("", TaskDomain::Worker, TaskPriority::Normal, std::forward<F>(callable));
    }

    template <typename F>
    TaskHandle emplaceSubflow(const char* name, F&& callable) {
        return emplaceSubflow(name, TaskDomain::Worker, TaskPriority::Normal, std::forward<F>(callable));
    }

    template <typename F>
    TaskHandle emplaceSubflow(const char* name, TaskDomain domain, F&& callable) {
        return emplaceSubflow(name, domain, TaskPriority::Normal, std::forward<F>(callable));
    }

    template <typename F>
    TaskHandle emplaceSubflow(const char* name, TaskDomain domain, TaskPriority priority, F&& callable) {
        LF_ASSERT(m_arena != nullptr);
        TaskNode* node = m_arena->create<TaskNode>();
        node->id = m_nodeCount++;
        node->name = name;
        node->domain = domain;
        node->priority = priority;
        node->graph = m_graph;
        node->type = TaskType::Subflow;

        node->setCallable([userFn = std::forward<F>(callable), node]() mutable {
            TaskScheduler* sched = (node->graph != nullptr) ? node->graph->scheduler() : nullptr;
            SlabArena* arena = (sched != nullptr) ? sched->currentWorkerArena() : nullptr;
            if (arena == nullptr && node->graph != nullptr) {
                arena = &node->graph->arena();
            }
            Subflow sub(node, arena);
            userFn(sub);
            sub.splice();
        }, m_arena);

        appendNode(node);
        return TaskHandle{node};
    }

    // --- Edge Construction ---

    void addEdge(TaskNode* from, TaskNode* to, int branch = -1) noexcept {
        LF_ASSERT(from != nullptr && to != nullptr && m_arena != nullptr);

        SuccessorEdge* edge = m_arena->create<SuccessorEdge>();
        edge->target = to;
        edge->branch = branch;
        edge->next = from->successors;
        from->successors = edge;

        to->initialInDegree++;
        to->inDegree.store(to->initialInDegree, std::memory_order_relaxed);
    }

    // --- Splicing & Execution Control ---

    /// Splices the subflow into the parent task graph.
    /// Connects sink tasks to downstream successors (or subflowExit barrier)
    /// and schedules subflow root tasks.
    void splice() noexcept {
        if (m_spliced) {
            return;
        }
        m_spliced = true;

        if (m_nodeCount == 0) {
            m_isDone.store(true, std::memory_order_release);
            return;
        }

        TaskNode* subflowExit = nullptr;

        if (!m_detached) {
            // Create the join barrier exit node for this subflow
            subflowExit = m_arena->create<TaskNode>();
            subflowExit->id = m_nodeCount + 1000000;
            subflowExit->name = "SubflowExit";
            subflowExit->domain = (m_parentNode != nullptr) ? m_parentNode->domain : TaskDomain::Worker;
            subflowExit->priority = (m_parentNode != nullptr) ? m_parentNode->priority : TaskPriority::Normal;
            subflowExit->graph = m_graph;
            if (m_joined) {
                subflowExit->setCallable([this]() noexcept {
                    m_isDone.store(true, std::memory_order_release);
                }, m_arena);
            } else {
                subflowExit->setCallable([]() noexcept {}, m_arena);
            }

            if (!m_joined && m_parentNode != nullptr) {
                // Asynchronous subflow: transfer parent's original successors to subflowExit
                subflowExit->successors = m_parentNode->successors;
                m_parentNode->successors = nullptr;
            }

            // Connect all subflow sink tasks to subflowExit
            TaskNode* curr = m_firstNode;
            while (curr != nullptr) {
                if (curr->successors == nullptr) {
                    addEdge(curr, subflowExit);
                }
                curr = curr->nextInGraph;
            }
        } else {
            m_isDone.store(true, std::memory_order_release);
        }

        // Account for pending tasks
        u32 totalSubflowTasks = (subflowExit != nullptr) ? (m_nodeCount + 1) : m_nodeCount;
        if (m_graph != nullptr) {
            m_graph->m_pendingTasks.fetch_add(totalSubflowTasks, std::memory_order_release);
        }

        // Dispatch root tasks (initialInDegree == 0)
        TaskNode* curr = m_firstNode;
        while (curr != nullptr) {
            if (curr->initialInDegree == 0) {
                if (m_graph != nullptr) {
                    m_graph->scheduleNode(curr);
                }
            }
            curr = curr->nextInGraph;
        }
    }

    /// Synchronously waits for the subflow to finish on the current thread using busy-helping.
    void join() noexcept {
        if (m_joined) {
            return;
        }
        m_joined = true;
        splice();

        if (m_nodeCount == 0) {
            return;
        }

        if (m_graph != nullptr && m_graph->scheduler() != nullptr) {
            m_graph->scheduler()->helpUntil([this]() noexcept {
                return isCompleted();
            });
        }
    }

    /// Detaches the subflow so parent successors do not wait for child tasks.
    void detach() noexcept {
        m_detached = true;
    }

    // --- Accessors ---

    LF_NODISCARD bool empty() const noexcept { return m_nodeCount == 0; }
    LF_NODISCARD usize size() const noexcept { return m_nodeCount; }
    LF_NODISCARD TaskNode* parentNode() const noexcept { return m_parentNode; }
    LF_NODISCARD TaskGraph* graph() const noexcept { return m_graph; }
    LF_NODISCARD SlabArena& arena() noexcept { return *m_arena; }
    LF_NODISCARD bool isCompleted() const noexcept {
        return m_isDone.load(std::memory_order_acquire);
    }

private:
    void appendNode(TaskNode* node) noexcept {
        LF_ASSERT(node != nullptr);
        if (m_lastNode != nullptr) {
            m_lastNode->nextInGraph = node;
        } else {
            m_firstNode = node;
        }
        m_lastNode = node;
    }

    TaskNode* m_parentNode{nullptr};
    TaskGraph* m_graph{nullptr};
    SlabArena* m_arena{nullptr};

    TaskNode* m_firstNode{nullptr};
    TaskNode* m_lastNode{nullptr};
    u32 m_nodeCount{0};

    std::atomic<bool> m_isDone{false};
    bool m_spliced{false};
    bool m_joined{false};
    bool m_detached{false};
};

template <typename F>
inline TaskHandle TaskGraph::emplaceSubflow(const char* name, TaskDomain domain, TaskPriority priority, F&& callable) {
    TaskNode* node = m_arena.create<TaskNode>();
    node->id = m_nodeCount++;
    node->name = name;
    node->domain = domain;
    node->priority = priority;
    node->graph = this;
    node->type = TaskType::Subflow;

    node->setCallable([userFn = std::forward<F>(callable), node]() mutable {
        TaskScheduler* sched = (node->graph != nullptr) ? node->graph->scheduler() : nullptr;
        SlabArena* arena = (sched != nullptr) ? sched->currentWorkerArena() : nullptr;
        if (arena == nullptr && node->graph != nullptr) {
            arena = &node->graph->arena();
        }
        Subflow sub(node, arena);
        userFn(sub);
        sub.splice();
    }, &m_arena);

    appendNode(node);
    return TaskHandle{node};
}

} // namespace lf
