#include <lightflow/task/task_graph.hpp>
#include <lightflow/scheduler/task_scheduler.hpp>
#include <lightflow/gpu/timeline_reactor.hpp>
#include <lightflow/core/tracy.hpp>

namespace lf {

// =============================================================================
// TaskNode Execution & Unblocking
// =============================================================================

void TaskNode::execute() noexcept {
    LF_ZONE_SCOPED_N("TaskNode::execute");

    state.store(TaskState::Executing, std::memory_order_relaxed);

    if (executeFn != nullptr) {
        executeFn(this);
    }

    state.store(TaskState::Completed, std::memory_order_release);

    unblockSuccessors();

    if (graph != nullptr) {
        graph->onTaskCompleted(this);
    }
}

void TaskNode::unblockSuccessors() noexcept {
    if (type == TaskType::Condition) {
        int chosenBranch = static_cast<int>(reinterpret_cast<intptr_t>(userData));
        SuccessorEdge* edge = successors;
        while (edge != nullptr) {
            TaskNode* succ = edge->target;
            if (succ != nullptr) {
                if (edge->branch == -1 || edge->branch == chosenBranch) {
                    succ->decrementActive(graph);
                } else {
                    succ->decrementSkipped(graph);
                }
            }
            edge = edge->next;
        }
    } else {
        SuccessorEdge* edge = successors;
        while (edge != nullptr) {
            TaskNode* succ = edge->target;
            if (succ != nullptr) {
                succ->decrementActive(graph);
            }
            edge = edge->next;
        }
    }
}

void TaskNode::decrementActive(TaskGraph* g) noexcept {
    u32 prev = inDegree.fetch_add(0x0000FFFFu, std::memory_order_acq_rel);
    u16 prevLower = static_cast<u16>(prev & 0xFFFFu);
    if (prevLower == 1) {
        if (g != nullptr) {
            g->scheduleNode(this);
        }
    }
}

void TaskNode::decrementSkipped(TaskGraph* g) noexcept {
    u32 prev = inDegree.fetch_sub(1u, std::memory_order_acq_rel);
    u16 prevLower = static_cast<u16>(prev & 0xFFFFu);
    if (prevLower == 1) {
        u32 newVal = prev - 1u;
        u16 upper = static_cast<u16>(newVal >> 16);
        if (upper > 0) {
            if (g != nullptr) {
                g->scheduleNode(this);
            }
        } else {
            cascadeSkip(g);
        }
    }
}

void TaskNode::cascadeSkip(TaskGraph* g) noexcept {
    state.store(TaskState::Skipped, std::memory_order_release);
    if (g != nullptr) {
        g->onTaskCompleted(this);
    }
    SuccessorEdge* edge = successors;
    while (edge != nullptr) {
        if (edge->target != nullptr) {
            edge->target->decrementSkipped(g);
        }
        edge = edge->next;
    }
}

TaskNode* TaskNode::waits(const TimelineSyncPoint& syncPoint) noexcept {
    if (graph != nullptr) {
        graph->addWaitPoint(this, syncPoint);
    }
    return this;
}

void TaskNode::handleGpuTimeout(TaskGraph* g) noexcept {
    state.store(TaskState::Timeout, std::memory_order_release);
    if (g != nullptr) {
        g->onTaskCompleted(this);
    }
    SuccessorEdge* edge = successors;
    while (edge != nullptr) {
        if (edge->target != nullptr) {
            edge->target->decrementSkipped(g);
        }
        edge = edge->next;
    }
}

// =============================================================================
// TaskGraph Implementation
// =============================================================================

TaskGraph::TaskGraph(BlockPool* pool)
    : m_arena(pool != nullptr ? *pool : BlockPool::global())
{
}

TaskGraph::~TaskGraph() noexcept {
    clear();
}

void TaskGraph::appendNode(TaskNode* node) noexcept {
    LF_ASSERT(node != nullptr);
    if (m_lastNode != nullptr) {
        m_lastNode->nextInGraph = node;
    } else {
        m_firstNode = node;
    }
    m_lastNode = node;
}

void TaskGraph::addEdge(TaskNode* from, TaskNode* to, int branch) noexcept {
    LF_ASSERT(from != nullptr && to != nullptr);

    SuccessorEdge* edge = m_arena.create<SuccessorEdge>();
    edge->target = to;
    edge->branch = branch;
    edge->next = from->successors;
    from->successors = edge;

    to->initialInDegree++;
    to->inDegree.store(to->initialInDegree, std::memory_order_relaxed);
}

void TaskGraph::addWaitPoint(TaskNode* node, const TimelineSyncPoint& syncPoint) noexcept {
    LF_ASSERT(node != nullptr);

    SlabArena* arena = nullptr;
    if (m_scheduler != nullptr) {
        arena = m_scheduler->currentWorkerArena();
    }
    if (arena == nullptr) {
        arena = &m_arena;
    }

    TimelineWaitNode* waitNode = arena->create<TimelineWaitNode>();
    waitNode->task = node;
    waitNode->syncPoint = syncPoint;
    waitNode->satisfied.store(false, std::memory_order_relaxed);

    node->initialInDegree++;
    node->inDegree.store(node->initialInDegree, std::memory_order_relaxed);

    if (m_scheduler != nullptr) {
        waitNode->registeredTime = std::chrono::steady_clock::now();
        m_scheduler->timelineReactor().registerWait(waitNode);
    } else {
        waitNode->nextInGraph = m_firstWaitNode;
        m_firstWaitNode = waitNode;
    }
}

void TaskGraph::prepareRun(TaskScheduler& scheduler) noexcept {
    LF_ZONE_SCOPED_N("TaskGraph::prepareRun");

    m_scheduler = &scheduler;
    m_pendingTasks.store(m_nodeCount, std::memory_order_release);

    if (m_nodeCount == 0) {
        return;
    }

    // Step 1: Inline Dual-State reset in a single fast traversal
    TaskNode* curr = m_firstNode;
    while (curr != nullptr) {
        curr->inDegree.store(curr->initialInDegree, std::memory_order_relaxed);
        curr->state.store(TaskState::Pending, std::memory_order_relaxed);
        curr = curr->nextInGraph;
    }

    // Step 2: Register all pending timeline wait nodes into scheduler's reactor
    auto now = std::chrono::steady_clock::now();
    TimelineWaitNode* waitNode = m_firstWaitNode;
    while (waitNode != nullptr) {
        waitNode->satisfied.store(false, std::memory_order_relaxed);
        waitNode->registeredTime = now;
        scheduler.timelineReactor().registerWait(waitNode);
        waitNode = waitNode->nextInGraph;
    }

    // Step 3: Schedule all root tasks (initialInDegree == 0)
    curr = m_firstNode;
    while (curr != nullptr) {
        if (curr->initialInDegree == 0) {
            scheduler.schedule(curr);
        }
        curr = curr->nextInGraph;
    }
}

void TaskGraph::scheduleNode(TaskNode* node) noexcept {
    if (m_scheduler != nullptr && node != nullptr) {
        m_scheduler->schedule(node);
    }
}

void TaskGraph::onTaskCompleted(TaskNode* node) noexcept {
    (void)node;
    m_pendingTasks.fetch_sub(1, std::memory_order_acq_rel);
}

void TaskGraph::clear() noexcept {
    LF_ZONE_SCOPED_N("TaskGraph::clear");

    // Destruct any non-trivial callables
    TaskNode* curr = m_firstNode;
    while (curr != nullptr) {
        TaskNode* next = curr->nextInGraph;
        curr->destroy();
        curr = next;
    }

    m_firstNode = nullptr;
    m_lastNode = nullptr;
    m_firstWaitNode = nullptr;
    m_nodeCount = 0;
    m_pendingTasks.store(0, std::memory_order_relaxed);
    m_scheduler = nullptr;

    // O(1) bulk memory reclamation back to the block pool
    m_arena.reset();
}

} // namespace lf
