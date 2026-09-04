#pragma once

#include <lightflow/core/types.hpp>
#include <lightflow/core/tracy.hpp>
#include <lightflow/scheduler/chase_lev_deque.hpp>
#include <lightflow/core/slab_arena.hpp>
#include <lightflow/gpu/timeline_reactor.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

namespace lf {

class TaskGraph;
class ITimelineDevice;

/// Configuration parameters for initializing a TaskScheduler instance.
struct SchedulerConfig {
    /// Number of worker threads. Defaults to 0 (which queries std::thread::hardware_concurrency()).
    /// Guaranteed to be at least 1.
    u32 workerCount{0};

    /// Prefix for worker thread names (e.g. "LF-Worker-0").
    std::string_view threadNamePrefix{"LF-Worker"};

    /// Optional CPU core affinity mapping for worker threads.
    /// Worker thread i is pinned to coreAffinity[i % coreAffinity.size()].
    std::span<const u32> coreAffinity{};

    /// Initial capacity for each worker's local Chase-Lev ring buffers.
    usize initialDequeCapacity{65536};

    /// Number of dedicated background IO threads.
    u32 ioWorkerCount{1};

    /// Prefix for IO worker thread names (e.g. "LF-IO-0").
    std::string_view ioThreadNamePrefix{"LF-IO"};

    /// Optional external timeline device interface for idle worker query fallback.
    ITimelineDevice* timelineDevice{nullptr};
};

/// Thread-local single-slot inline continuation task.
/// Bypasses deque push/pop and delivers 100% L1 cache hits for serial task dependencies.
inline thread_local TaskNode* t_nextInlineTask{nullptr};

/// Flag indicating whether the calling thread is actively executing a task loop (workerLoop or helpUntil).
inline thread_local bool t_canInlineTask{false};

/// Lock-free, wait-free bounded Multi-Producer Multi-Consumer (MPMC) queue
/// based on Dmitry Vyukov's array-based algorithm.
/// Guarantees zero allocations during steady-state dispatching and complete ABA immunity.
class alignas(lf::CACHELINE_SIZE) MpmcTaskQueue {
public:
    explicit MpmcTaskQueue(usize capacity = 2048);
    ~MpmcTaskQueue();

    MpmcTaskQueue(const MpmcTaskQueue&) = delete;
    MpmcTaskQueue& operator=(const MpmcTaskQueue&) = delete;
    MpmcTaskQueue(MpmcTaskQueue&&) = delete;
    MpmcTaskQueue& operator=(MpmcTaskQueue&&) = delete;

    /// Attempts to enqueue a task node. Returns true on success, false if the queue is full.
    bool tryEnqueue(TaskNode* task) noexcept;

    /// Attempts to dequeue a task node. Returns nullptr if the queue is empty.
    LF_NODISCARD TaskNode* tryDequeue() noexcept;

    /// Returns true if the queue is currently empty.
    LF_NODISCARD bool empty() const noexcept;

    /// Returns the approximate count of elements currently in the queue.
    LF_NODISCARD usize size() const noexcept;

    /// Returns the capacity of the queue buffer.
    LF_NODISCARD usize capacity() const noexcept { return m_capacity; }

private:
    struct Cell {
        std::atomic<usize> sequence;
        TaskNode* task;
    };

    usize m_capacity;
    usize m_mask;
    Cell* m_buffer{nullptr};

    LF_ALIGN_CACHELINE std::atomic<usize> m_enqueuePos{0};
    LF_ALIGN_CACHELINE std::atomic<usize> m_dequeuePos{0};
};

/// High-performance C++23 task graph engine executor.
/// Manages a persistent worker thread pool, two-tier adaptive spin + OS futex parking,
/// execution domain routing (Worker, MainThread, IO), and stackless helping wait loops.
class alignas(lf::CACHELINE_SIZE) TaskScheduler {
public:
    explicit TaskScheduler(const SchedulerConfig& config = {});
    ~TaskScheduler();

    TaskScheduler(const TaskScheduler&) = delete;
    TaskScheduler& operator=(const TaskScheduler&) = delete;
    TaskScheduler(TaskScheduler&&) = delete;
    TaskScheduler& operator=(TaskScheduler&&) = delete;

    // --- Task Dispatching ---

    /// Schedules a task into the engine according to its embedded domain and priority.
    void schedule(TaskNode* task, bool notify = true) noexcept;

    /// Schedules a task into the engine overriding its execution domain.
    void schedule(TaskNode* task, TaskDomain domain, bool notify = true) noexcept;

    /// Schedules a batch of tasks into the engine.
    void scheduleBatch(std::span<TaskNode*> tasks) noexcept;

    /// Wakes up sleeping workers to handle newly scheduled work.
    void notifyWorker(u32 count = 1) noexcept;

    // --- GPU Timeline Synchronization ---

    /// Fast-path push notification unblocking waiting GPU timeline tasks with 0 ns latency.
    void notifyTimelineAdvanced(TimelineHandle handle, u64 completedValue) noexcept;

    /// Registers an external timeline device interface for idle worker query fallback.
    void setTimelineDevice(ITimelineDevice* device) noexcept { m_timelineDevice = device; }

    /// Returns the registered external timeline device, or nullptr if none.
    LF_NODISCARD ITimelineDevice* timelineDevice() const noexcept { return m_timelineDevice; }

    /// Provides direct access to the embedded GPU timeline reactor.
    LF_NODISCARD TimelineReactor& timelineReactor() noexcept { return m_timelineReactor; }
    LF_NODISCARD const TimelineReactor& timelineReactor() const noexcept { return m_timelineReactor; }

    // --- Execution Domain Management ---

    /// Drains and executes all pending MainThread domain tasks on the calling thread.
    /// Tasks are executed in strict FIFO submission order.
    /// Returns the number of tasks executed.
    usize drainMainThreadTasks() noexcept;

    /// Returns the approximate number of pending tasks in the MainThread domain queue.
    LF_NODISCARD usize mainThreadTaskCount() const noexcept;

    // --- Stackless Helping Loops ---

    /// Stackless busy-helping loop: steals and executes available tasks across the system
    /// until the provided predicate returns true. Never blocks the host OS thread.
    template <typename Predicate>
    Status helpUntil(Predicate&& isDone) noexcept {
        u32 workerIdx = currentWorkerIndex();
        bool prevCanInline = t_canInlineTask;
        t_canInlineTask = true;
        while (!isDone()) {
            TaskNode* task = (workerIdx != INVALID_WORKER_INDEX)
                ? findWork(workerIdx)
                : findWorkExternal();

            if (task != nullptr) {
                task->execute();
                while (t_nextInlineTask != nullptr) {
                    TaskNode* nextTask = t_nextInlineTask;
                    t_nextInlineTask = nullptr;
                    nextTask->execute();
                }
            } else {
                pollTimelineFallback();
                cpu_pause();
            }
        }
        t_canInlineTask = prevCanInline;
        return m_timelineReactor.hasTimeout() ? Status::GpuTimeout : Status::Success;
    }

    /// Stackless busy-helping wait: helps execute tasks until pendingTasks decrements to zero.
    Status runAndWait(std::atomic<u32>& pendingTasks) noexcept {
        return helpUntil([&pendingTasks]() noexcept {
            return pendingTasks.load(std::memory_order_acquire) == 0;
        });
    }

    /// Stackless busy-helping wait on any graph type implementing `bool isCompleted() const noexcept`.
    template <typename GraphType>
        requires requires(const GraphType& g) { { g.isCompleted() } -> std::same_as<bool>; }
    Status runAndWait(const GraphType& graph) noexcept {
        return helpUntil([&graph]() noexcept {
            return graph.isCompleted();
        });
    }

    /// Stackless busy-helping execution and wait on a TaskGraph.
    /// Prepares the graph, dispatches root tasks, and helps execute until complete.
    Status runAndWait(TaskGraph& graph) noexcept;

    // --- Lifecycle & Metrics ---

    /// Gracefully shuts down the scheduler, unparking all workers and joining threads.
    /// Idempotent.
    void shutdown() noexcept;

    /// Returns true if the scheduler has been requested to shut down.
    LF_NODISCARD bool isShutdown() const noexcept;

    /// Returns the number of worker threads in the pool.
    LF_NODISCARD u32 workerCount() const noexcept;

    /// Returns the number of IO worker threads in the pool.
    LF_NODISCARD u32 ioWorkerCount() const noexcept;

    /// Returns the worker index of the calling thread (0 .. workerCount - 1),
    /// or INVALID_WORKER_INDEX if the calling thread is not a worker of this scheduler.
    LF_NODISCARD u32 currentWorkerIndex() const noexcept;

    /// Returns true if the calling thread is a registered worker thread of this scheduler.
    LF_NODISCARD bool isWorkerThread() const noexcept;

    /// Returns the number of workers currently parked in Tier 2 futex sleep.
    LF_NODISCARD u32 sleepingWorkerCount() const noexcept;

    /// Provides access to the local DualPriorityQueue for a specific worker.
    LF_NODISCARD DualPriorityQueue& workerDeque(u32 workerIndex) noexcept;

    /// Returns the SlabArena dedicated to the current worker thread, or nullptr if called from an external thread.
    LF_NODISCARD SlabArena* currentWorkerArena() noexcept;

    /// Resets all worker arenas in O(1), returning chained slabs to BlockPool.
    void resetWorkerArenas() noexcept;

    static constexpr u32 INVALID_WORKER_INDEX = lf::INVALID_WORKER_INDEX;

private:
    struct alignas(lf::CACHELINE_SIZE) WorkerState {
        DualPriorityQueue deque;
        u32 stealVictimSeed{0};
        SlabArena arena;

        static inline usize s_defaultInitialCapacity{65536};

        WorkerState()
            : deque(s_defaultInitialCapacity), arena(BlockPool::global())
        {}

        explicit WorkerState(usize initialCapacity)
            : deque(initialCapacity), arena(BlockPool::global())
        {}
    };

    void workerLoop(u32 workerIndex) noexcept;
    void ioWorkerLoop(u32 ioIndex) noexcept;

    LF_NODISCARD TaskNode* findWork(u32 workerIndex) noexcept;
    LF_NODISCARD TaskNode* findWorkExternal() noexcept;

    void pushMainThreadTask(TaskNode* task) noexcept;

    SchedulerConfig m_config;
    u32 m_workerCount{0};
    u32 m_ioWorkerCount{0};

    // Worker threads and their isolated local deques
    std::unique_ptr<WorkerState[]> m_workers;
    std::vector<std::thread> m_workerThreads;

    // IO threads
    std::vector<std::thread> m_ioThreads;

    // External injection queues (Multi-Producer, Multi-Consumer)
    MpmcTaskQueue m_globalHigh;
    MpmcTaskQueue m_globalNormal;

    // IO task queue
    MpmcTaskQueue m_ioQueue;

    // MainThread MPSC intrusive queue
    LF_ALIGN_CACHELINE std::atomic<TaskNode*> m_mainThreadHead{nullptr};
    LF_ALIGN_CACHELINE std::atomic<usize> m_mainThreadCount{0};

    // Two-Tier Parking Protocol: Futex & Sleeping counter
    LF_ALIGN_CACHELINE std::atomic<u32> m_wakeSignal{0};
    LF_ALIGN_CACHELINE std::atomic<u32> m_sleepingCount{0};

    // IO thread futex wake signal
    LF_ALIGN_CACHELINE std::atomic<u32> m_ioWakeSignal{0};

    // Global shutdown flag
    LF_ALIGN_CACHELINE std::atomic<bool> m_stop{false};

    // External thread steal seed
    LF_ALIGN_CACHELINE std::atomic<u32> m_externalStealSeed{0};

    // GPU Timeline Synchronization
    void pollTimelineFallback() noexcept;
    TimelineReactor m_timelineReactor;
    ITimelineDevice* m_timelineDevice{nullptr};
};

} // namespace lf
