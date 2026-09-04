#include <lightflow/task/task_graph.hpp>
#include <lightflow/scheduler/task_scheduler.hpp>
#include <lightflow/gpu/timeline_reactor.hpp>
#include <lightflow/core/tracy.hpp>

namespace lf {

namespace {

bool reaches(TaskNode* start, TaskNode* target) noexcept {
    if (start == nullptr || target == nullptr) {
        return false;
    }
    if (start == target) {
        return true;
    }
    constexpr usize MAX_VISITED = 128;
    TaskNode* visited[MAX_VISITED]{};
    usize visitedCount = 0;

    TaskNode* queue[MAX_VISITED]{};
    usize head = 0;
    usize tail = 0;
    queue[tail++] = start;
    visited[visitedCount++] = start;

    while (head < tail) {
        TaskNode* curr = queue[head++];
        if (curr == target) {
            return true;
        }
        for (SuccessorEdge* e = curr->successors; e != nullptr; e = e->next) {
            TaskNode* succ = e->target;
            if (succ == target) {
                return true;
            }
            if (succ != nullptr) {
                bool alreadyVisited = false;
                for (usize i = 0; i < visitedCount; ++i) {
                    if (visited[i] == succ) {
                        alreadyVisited = true;
                        break;
                    }
                }
                if (!alreadyVisited) {
                    LF_ASSERT(tail < MAX_VISITED && visitedCount < MAX_VISITED);
                    if (tail < MAX_VISITED && visitedCount < MAX_VISITED) {
                        visited[visitedCount++] = succ;
                        queue[tail++] = succ;
                    }
                }
            }
        }
    }
    return false;
}

} // anonymous namespace

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

        // Check if the chosen branch is a loop-back edge
        SuccessorEdge* chosenLoopEdge = nullptr;
        for (SuccessorEdge* edge = successors; edge != nullptr; edge = edge->next) {
            if ((edge->branch == -1 || edge->branch == chosenBranch) && edge->isLoopBack) {
                chosenLoopEdge = edge;
                break;
            }
        }

        if (chosenLoopEdge != nullptr) {
            TaskNode* target = chosenLoopEdge->target;

            // Collect all nodes participating in the cycle from target to this
            constexpr usize MAX_CYCLE = 64;
            TaskNode* cycleNodes[MAX_CYCLE]{};
            usize cycleCount = 0;

            if (target == this) {
                cycleNodes[cycleCount++] = this;
            } else {
                TaskNode* queue[MAX_CYCLE]{};
                usize head = 0;
                usize tail = 0;
                queue[tail++] = target;
                cycleNodes[cycleCount++] = target;

                while (head < tail) {
                    TaskNode* curr = queue[head++];
                    for (SuccessorEdge* e = curr->successors; e != nullptr; e = e->next) {
                        TaskNode* succ = e->target;
                        if (succ != nullptr && !e->isLoopBack && reaches(succ, this)) {
                            bool visited = false;
                            for (usize i = 0; i < cycleCount; ++i) {
                                if (cycleNodes[i] == succ) {
                                    visited = true;
                                    break;
                                }
                            }
                            if (!visited) {
                                LF_ASSERT(tail < MAX_CYCLE && cycleCount < MAX_CYCLE);
                                if (tail < MAX_CYCLE && cycleCount < MAX_CYCLE) {
                                    cycleNodes[cycleCount++] = succ;
                                    queue[tail++] = succ;
                                }
                            }
                        }
                    }
                }
            }

            // Reset inDegrees and pending state for cycle nodes
            for (usize i = 0; i < cycleCount; ++i) {
                TaskNode* n = cycleNodes[i];
                if (n != target) {
                    n->inDegree.store(n->initialInDegree, std::memory_order_relaxed);
                }
                n->state.store(TaskState::Pending, std::memory_order_relaxed);
            }

            if (graph != nullptr) {
                graph->addPendingTasks(static_cast<u32>(cycleCount));
                graph->scheduleNode(target);
            }
        } else {
            SuccessorEdge* edge = successors;
            u32 unblockedCount = 0;
            while (edge != nullptr) {
                TaskNode* succ = edge->target;
                if (succ != nullptr) {
                    if (edge->branch == -1 || edge->branch == chosenBranch) {
                        if (succ->decrementActive()) {
                            if (t_canInlineTask && t_nextInlineTask == nullptr && succ->domain == TaskDomain::Worker) {
                                t_nextInlineTask = succ;
                            } else if (graph != nullptr) {
                                graph->scheduleNode(succ, /*notify=*/false);
                                if (succ->domain == TaskDomain::Worker) {
                                    unblockedCount++;
                                    if (unblockedCount >= 64) {
                                        if (graph->scheduler() != nullptr) {
                                            graph->scheduler()->notifyWorker(unblockedCount);
                                        }
                                        unblockedCount = 0;
                                    }
                                }
                            }
                        }
                    } else if (!edge->isLoopBack) {
                        succ->decrementSkipped(graph);
                    }
                }
                edge = edge->next;
            }
            if (unblockedCount > 0 && graph != nullptr && graph->scheduler() != nullptr) {
                graph->scheduler()->notifyWorker(unblockedCount);
            }
        }
    } else {
        SuccessorEdge* edge = successors;
        u32 unblockedCount = 0;
        while (edge != nullptr) {
            TaskNode* succ = edge->target;
            if (succ != nullptr) {
                if (succ->decrementActive()) {
                    if (t_canInlineTask && t_nextInlineTask == nullptr && succ->domain == TaskDomain::Worker) {
                        t_nextInlineTask = succ;
                    } else if (graph != nullptr) {
                        graph->scheduleNode(succ, /*notify=*/false);
                        if (succ->domain == TaskDomain::Worker) {
                            unblockedCount++;
                            if (unblockedCount >= 64) {
                                if (graph->scheduler() != nullptr) {
                                    graph->scheduler()->notifyWorker(unblockedCount);
                                }
                                unblockedCount = 0;
                            }
                        }
                    }
                }
            }
            edge = edge->next;
        }
        if (unblockedCount > 0 && graph != nullptr && graph->scheduler() != nullptr) {
            graph->scheduler()->notifyWorker(unblockedCount);
        }
    }
}

bool TaskNode::decrementActive() noexcept {
    u32 prev = inDegree.fetch_add(0x0000FFFFu, std::memory_order_release);
    u16 prevLower = static_cast<u16>(prev & 0xFFFFu);
    if (prevLower == 1) {
        std::atomic_thread_fence(std::memory_order_acquire);
        return true;
    }
    return false;
}

void TaskNode::decrementActive(TaskGraph* g) noexcept {
    if (decrementActive()) {
        if (g != nullptr) {
            g->scheduleNode(this);
        }
    }
}

void TaskNode::decrementSkipped(TaskGraph* g) noexcept {
    u32 prev = inDegree.fetch_sub(1u, std::memory_order_release);
    u16 prevLower = static_cast<u16>(prev & 0xFFFFu);
    if (prevLower == 1) {
        std::atomic_thread_fence(std::memory_order_acquire);
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

    bool isLoop = (from->type == TaskType::Condition || branch >= 0) && reaches(to, from);
    edge->isLoopBack = isLoop;

    if (!isLoop) {
        to->initialInDegree++;
        to->inDegree.store(to->initialInDegree, std::memory_order_relaxed);
    }
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
    u32 rootWorkerCount = 0;
    while (curr != nullptr) {
        if (curr->initialInDegree == 0) {
            scheduler.schedule(curr, /*notify=*/false);
            if (curr->domain == TaskDomain::Worker) {
                rootWorkerCount++;
                if (rootWorkerCount >= 64) {
                    scheduler.notifyWorker(rootWorkerCount);
                    rootWorkerCount = 0;
                }
            }
        }
        curr = curr->nextInGraph;
    }
    if (rootWorkerCount > 0) {
        scheduler.notifyWorker(rootWorkerCount);
    }
}

void TaskGraph::scheduleNode(TaskNode* node, bool notify) noexcept {
    if (m_scheduler != nullptr && node != nullptr) {
        m_scheduler->schedule(node, notify);
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
