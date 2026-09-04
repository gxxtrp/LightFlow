#include <lightflow/scheduler/task_scheduler.hpp>
#include <lightflow/task/task_graph.hpp>
#include <lightflow/core/tracy.hpp>

#include <algorithm>
#include <bit>
#include <cstdio>
#include <new>

#if defined(__APPLE__)
    #include <pthread.h>
    #include <mach/mach.h>
    #include <mach/thread_policy.h>
#elif defined(__linux__)
    #include <pthread.h>
    #include <sched.h>
#elif defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#endif

namespace lf {

namespace {

thread_local u32 t_workerIndex{INVALID_WORKER_INDEX};
thread_local const TaskScheduler* t_currentScheduler{nullptr};

LF_FORCE_INLINE u32 xorshift32(u32& state) noexcept {
    u32 x = state;
    if (x == 0) {
        x = 0x6D2B79F5u;
    }
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    state = x;
    return x;
}

void set_thread_name(const char* name) noexcept {
    LF_SET_THREAD_NAME(name);
#if defined(__APPLE__)
    pthread_setname_np(name);
#elif defined(__linux__)
    pthread_setname_np(pthread_self(), name);
#elif defined(_WIN32)
    // Optional Windows thread name API
    (void)name;
#else
    (void)name;
#endif
}

bool set_thread_affinity(u32 coreId) noexcept {
#if defined(__APPLE__)
    thread_affinity_policy_data_t policy{
        .affinity_tag = static_cast<integer_t>(coreId + 1)
    };
    return thread_policy_set(
        mach_thread_self(),
        THREAD_AFFINITY_POLICY,
        reinterpret_cast<thread_policy_t>(&policy),
        THREAD_AFFINITY_POLICY_COUNT
    ) == KERN_SUCCESS;
#elif defined(__linux__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(coreId, &cpuset);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0;
#elif defined(_WIN32)
    return SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(1ULL << (coreId % 64))) != 0;
#else
    (void)coreId;
    return false;
#endif
}

} // anonymous namespace

// --- MpmcTaskQueue Implementation ---

MpmcTaskQueue::MpmcTaskQueue(usize capacity) {
    m_capacity = std::bit_ceil(std::max<usize>(capacity, 16));
    m_mask = m_capacity - 1;
    m_buffer = new (std::nothrow) Cell[m_capacity];
    LF_ASSERT(m_buffer != nullptr);

    for (usize i = 0; i < m_capacity; ++i) {
        m_buffer[i].sequence.store(i, std::memory_order_relaxed);
        m_buffer[i].task = nullptr;
    }
    m_enqueuePos.store(0, std::memory_order_relaxed);
    m_dequeuePos.store(0, std::memory_order_relaxed);
}

MpmcTaskQueue::~MpmcTaskQueue() {
    delete[] m_buffer;
}

bool MpmcTaskQueue::tryEnqueue(TaskNode* task) noexcept {
    Cell* cell = nullptr;
    usize pos = m_enqueuePos.load(std::memory_order_relaxed);
    for (;;) {
        cell = &m_buffer[pos & m_mask];
        usize seq = cell->sequence.load(std::memory_order_acquire);
        intptr_t dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
        if (dif == 0) {
            if (m_enqueuePos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                break;
            }
        } else if (dif < 0) {
            return false;
        } else {
            pos = m_enqueuePos.load(std::memory_order_relaxed);
        }
    }
    cell->task = task;
    cell->sequence.store(pos + 1, std::memory_order_release);
    return true;
}

TaskNode* MpmcTaskQueue::tryDequeue() noexcept {
    Cell* cell = nullptr;
    usize pos = m_dequeuePos.load(std::memory_order_relaxed);
    for (;;) {
        cell = &m_buffer[pos & m_mask];
        usize seq = cell->sequence.load(std::memory_order_acquire);
        intptr_t dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
        if (dif == 0) {
            if (m_dequeuePos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                break;
            }
        } else if (dif < 0) {
            return nullptr;
        } else {
            pos = m_dequeuePos.load(std::memory_order_relaxed);
        }
    }
    TaskNode* task = cell->task;
    cell->task = nullptr;
    cell->sequence.store(pos + m_mask + 1, std::memory_order_release);
    return task;
}

bool MpmcTaskQueue::empty() const noexcept {
    usize enq = m_enqueuePos.load(std::memory_order_relaxed);
    usize deq = m_dequeuePos.load(std::memory_order_relaxed);
    return enq <= deq;
}

usize MpmcTaskQueue::size() const noexcept {
    usize enq = m_enqueuePos.load(std::memory_order_relaxed);
    usize deq = m_dequeuePos.load(std::memory_order_relaxed);
    return (enq > deq) ? (enq - deq) : 0;
}

// --- TaskScheduler Implementation ---

TaskScheduler::TaskScheduler(const SchedulerConfig& config)
    : m_config(config)
    , m_globalHigh(2048)
    , m_globalNormal(4096)
    , m_ioQueue(1024)
    , m_timelineDevice(config.timelineDevice)
{
    u32 hw = std::thread::hardware_concurrency();
    m_workerCount = (config.workerCount > 0) ? config.workerCount : ((hw > 0) ? hw : 1);
    m_ioWorkerCount = config.ioWorkerCount;

    // Allocate contiguous aligned WorkerState array
    WorkerState::s_defaultInitialCapacity = (config.initialDequeCapacity > 0) ? config.initialDequeCapacity : 1024;
    m_workers = std::make_unique<WorkerState[]>(m_workerCount);
    for (u32 i = 0; i < m_workerCount; ++i) {
        m_workers[i].stealVictimSeed = (i + 1) * 2654435761u;
    }

    // Launch worker threads
    m_workerThreads.reserve(m_workerCount);
    for (u32 i = 0; i < m_workerCount; ++i) {
        m_workerThreads.emplace_back(&TaskScheduler::workerLoop, this, i);
    }

    // Launch IO worker threads
    m_ioThreads.reserve(m_ioWorkerCount);
    for (u32 i = 0; i < m_ioWorkerCount; ++i) {
        m_ioThreads.emplace_back(&TaskScheduler::ioWorkerLoop, this, i);
    }
}

TaskScheduler::~TaskScheduler() {
    shutdown();
}

void TaskScheduler::schedule(TaskNode* task) noexcept {
    LF_ASSERT(task != nullptr);
    schedule(task, task->domain);
}

void TaskScheduler::schedule(TaskNode* task, TaskDomain domain) noexcept {
    LF_ASSERT(task != nullptr);
    task->domain = domain;

    switch (domain) {
        case TaskDomain::MainThread:
            pushMainThreadTask(task);
            break;

        case TaskDomain::IO:
            while (!m_ioQueue.tryEnqueue(task)) {
                cpu_pause();
            }
            m_ioWakeSignal.fetch_add(1, std::memory_order_release);
            m_ioWakeSignal.notify_one();
            break;

        case TaskDomain::Worker: {
            u32 workerIdx = currentWorkerIndex();
            if (workerIdx != INVALID_WORKER_INDEX) {
                // Producer is worker thread: push directly to local deque
                m_workers[workerIdx].deque.push(task, task->priority);
            } else {
                // Producer is external thread: push to global injection queue
                auto& queue = (task->priority == TaskPriority::High) ? m_globalHigh : m_globalNormal;
                while (!queue.tryEnqueue(task)) {
                    cpu_pause();
                }
            }
            notifyWorker(1);
            break;
        }
    }
}

void TaskScheduler::scheduleBatch(std::span<TaskNode*> tasks) noexcept {
    if (tasks.empty()) {
        return;
    }

    u32 workerTaskCount = 0;
    for (TaskNode* task : tasks) {
        if (task == nullptr) {
            continue;
        }

        switch (task->domain) {
            case TaskDomain::MainThread:
                pushMainThreadTask(task);
                break;

            case TaskDomain::IO:
                while (!m_ioQueue.tryEnqueue(task)) {
                    cpu_pause();
                }
                m_ioWakeSignal.fetch_add(1, std::memory_order_release);
                m_ioWakeSignal.notify_one();
                break;

            case TaskDomain::Worker: {
                u32 workerIdx = currentWorkerIndex();
                if (workerIdx != INVALID_WORKER_INDEX) {
                    m_workers[workerIdx].deque.push(task, task->priority);
                } else {
                    auto& queue = (task->priority == TaskPriority::High) ? m_globalHigh : m_globalNormal;
                    while (!queue.tryEnqueue(task)) {
                        cpu_pause();
                    }
                }
                workerTaskCount++;
                break;
            }
        }
    }

    if (workerTaskCount > 0) {
        notifyWorker(workerTaskCount);
    }
}

void TaskScheduler::notifyTimelineAdvanced(TimelineHandle handle, u64 completedValue) noexcept {
    usize unblocked = m_timelineReactor.notifyTimelineAdvanced(handle, completedValue);
    if (unblocked > 0) {
        notifyWorker(static_cast<u32>(unblocked));
    }
}

void TaskScheduler::pollTimelineFallback() noexcept {
    if (m_timelineReactor.hasPending()) {
        if (m_timelineDevice != nullptr) {
            usize unblocked = m_timelineReactor.pollDevice(*m_timelineDevice);
            if (unblocked > 0) {
                notifyWorker(static_cast<u32>(unblocked));
            }
        }
        if (m_timelineReactor.checkTimeouts()) {
            notifyWorker(m_workerCount);
        }
    }
}

usize TaskScheduler::drainMainThreadTasks() noexcept {
    TaskNode* head = m_mainThreadHead.exchange(nullptr, std::memory_order_acq_rel);
    if (head == nullptr) {
        return 0;
    }

    // Reverse the singly-linked list to preserve FIFO submission order
    TaskNode* prev = nullptr;
    TaskNode* curr = head;
    usize count = 0;
    while (curr != nullptr) {
        TaskNode* next = curr->next;
        curr->next = prev;
        prev = curr;
        curr = next;
        ++count;
    }

    // Execute all tasks sequentially on the main thread
    curr = prev;
    while (curr != nullptr) {
        TaskNode* next = curr->next;
        curr->next = nullptr;
        curr->execute();
        curr = next;
    }

    m_mainThreadCount.fetch_sub(count, std::memory_order_relaxed);
    return count;
}

usize TaskScheduler::mainThreadTaskCount() const noexcept {
    return m_mainThreadCount.load(std::memory_order_relaxed);
}

void TaskScheduler::shutdown() noexcept {
    if (m_stop.exchange(true, std::memory_order_acq_rel)) {
        return; // Already shut down
    }

    // Wake all parked workers
    m_wakeSignal.fetch_add(1, std::memory_order_seq_cst);
    m_wakeSignal.notify_all();

    // Wake all IO workers
    m_ioWakeSignal.fetch_add(1, std::memory_order_release);
    m_ioWakeSignal.notify_all();

    // Join all worker threads
    for (auto& t : m_workerThreads) {
        if (t.joinable()) {
            t.join();
        }
    }
    m_workerThreads.clear();

    // Join all IO threads
    for (auto& t : m_ioThreads) {
        if (t.joinable()) {
            t.join();
        }
    }
    m_ioThreads.clear();
}

bool TaskScheduler::isShutdown() const noexcept {
    return m_stop.load(std::memory_order_relaxed);
}

u32 TaskScheduler::workerCount() const noexcept {
    return m_workerCount;
}

u32 TaskScheduler::ioWorkerCount() const noexcept {
    return m_ioWorkerCount;
}

u32 TaskScheduler::currentWorkerIndex() const noexcept {
    return (t_currentScheduler == this) ? t_workerIndex : INVALID_WORKER_INDEX;
}

bool TaskScheduler::isWorkerThread() const noexcept {
    return currentWorkerIndex() != INVALID_WORKER_INDEX;
}

u32 TaskScheduler::sleepingWorkerCount() const noexcept {
    return m_sleepingCount.load(std::memory_order_relaxed);
}

DualPriorityQueue& TaskScheduler::workerDeque(u32 workerIndex) noexcept {
    LF_ASSERT(workerIndex < m_workerCount);
    return m_workers[workerIndex].deque;
}

void TaskScheduler::workerLoop(u32 workerIndex) noexcept {
    t_workerIndex = workerIndex;
    t_currentScheduler = this;

    char nameBuf[32];
    std::snprintf(nameBuf, sizeof(nameBuf), "%.*s-%u",
        static_cast<int>(m_config.threadNamePrefix.size()),
        m_config.threadNamePrefix.data(),
        workerIndex);
    set_thread_name(nameBuf);

    if (!m_config.coreAffinity.empty()) {
        u32 coreId = m_config.coreAffinity[workerIndex % m_config.coreAffinity.size()];
        set_thread_affinity(coreId);
    }

    constexpr u32 SPIN_COUNT = 64;

    while (!m_stop.load(std::memory_order_acquire)) {
        TaskNode* task = findWork(workerIndex);
        if (task != nullptr) {
            task->execute();
            continue;
        }

        // Tier 1: Adaptive pause-spinning
        bool foundDuringSpin = false;
        for (u32 spin = 0; spin < SPIN_COUNT; ++spin) {
            cpu_pause();
            task = findWork(workerIndex);
            if (task != nullptr) {
                foundDuringSpin = true;
                break;
            }
        }
        if (foundDuringSpin && task != nullptr) {
            task->execute();
            continue;
        }

        // Steal-backoff fallback device polling and watchdog timeout check
        pollTimelineFallback();
        task = findWork(workerIndex);
        if (task != nullptr) {
            task->execute();
            continue;
        }

        // Tier 2: Low-overhead OS futex sleep
        m_sleepingCount.fetch_add(1, std::memory_order_seq_cst);
        u32 currentSignal = m_wakeSignal.load(std::memory_order_seq_cst);

        // Double-check conditions before parking
        if (m_stop.load(std::memory_order_acquire)) {
            m_sleepingCount.fetch_sub(1, std::memory_order_relaxed);
            break;
        }

        task = findWork(workerIndex);
        if (task != nullptr) {
            m_sleepingCount.fetch_sub(1, std::memory_order_relaxed);
            task->execute();
            continue;
        }

        // Park thread on OS futex
        m_wakeSignal.wait(currentSignal, std::memory_order_relaxed);

        // Awakened
        m_sleepingCount.fetch_sub(1, std::memory_order_relaxed);
    }

    // Drain remaining tasks in local deque on shutdown
    while (TaskNode* task = m_workers[workerIndex].deque.pop()) {
        task->execute();
    }
}

void TaskScheduler::ioWorkerLoop(u32 ioIndex) noexcept {
    char nameBuf[32];
    std::snprintf(nameBuf, sizeof(nameBuf), "%.*s-%u",
        static_cast<int>(m_config.ioThreadNamePrefix.size()),
        m_config.ioThreadNamePrefix.data(),
        ioIndex);
    set_thread_name(nameBuf);

    while (!m_stop.load(std::memory_order_acquire)) {
        TaskNode* task = m_ioQueue.tryDequeue();
        if (task != nullptr) {
            task->execute();
            continue;
        }

        u32 currentSignal = m_ioWakeSignal.load(std::memory_order_acquire);
        if (m_stop.load(std::memory_order_acquire)) {
            break;
        }
        if (!m_ioQueue.empty()) {
            continue;
        }

        m_ioWakeSignal.wait(currentSignal, std::memory_order_relaxed);
    }

    // Drain remaining IO tasks
    while (TaskNode* task = m_ioQueue.tryDequeue()) {
        task->execute();
    }
}

TaskNode* TaskScheduler::findWork(u32 workerIndex) noexcept {
    // 1. Local deque (High first, then Normal)
    TaskNode* task = m_workers[workerIndex].deque.pop();
    if (task != nullptr) {
        return task;
    }

    // 2. Global injection queue (High first, then Normal)
    task = m_globalHigh.tryDequeue();
    if (task != nullptr) {
        return task;
    }
    task = m_globalNormal.tryDequeue();
    if (task != nullptr) {
        return task;
    }

    // 3. Steal from other workers (batch steal, fallback to single steal)
    u32 seed = m_workers[workerIndex].stealVictimSeed;
    u32 startIdx = xorshift32(seed) % m_workerCount;
    m_workers[workerIndex].stealVictimSeed = seed;

    for (u32 attempt = 0; attempt < m_workerCount; ++attempt) {
        u32 victim = (startIdx + attempt) % m_workerCount;
        if (victim == workerIndex) {
            continue;
        }

        // Try batch steal into caller's local deque
        usize stolen = m_workers[victim].deque.stealBatch(m_workers[workerIndex].deque);
        if (stolen > 0) {
            return m_workers[workerIndex].deque.pop();
        }

        // Try single steal
        task = m_workers[victim].deque.steal();
        if (task != nullptr) {
            return task;
        }
    }

    return nullptr;
}

TaskNode* TaskScheduler::findWorkExternal() noexcept {
    // 1. Global injection queue
    TaskNode* task = m_globalHigh.tryDequeue();
    if (task != nullptr) {
        return task;
    }
    task = m_globalNormal.tryDequeue();
    if (task != nullptr) {
        return task;
    }

    // 2. Steal single task from workers
    u32 seed = m_externalStealSeed.fetch_add(1, std::memory_order_relaxed);
    u32 startIdx = (seed * 2654435761u) % m_workerCount;

    for (u32 attempt = 0; attempt < m_workerCount; ++attempt) {
        u32 victim = (startIdx + attempt) % m_workerCount;
        task = m_workers[victim].deque.steal();
        if (task != nullptr) {
            return task;
        }
    }

    return nullptr;
}

void TaskScheduler::notifyWorker(u32 count) noexcept {
    if (m_sleepingCount.load(std::memory_order_seq_cst) > 0) {
        if (count == 1) {
            m_wakeSignal.fetch_add(1, std::memory_order_seq_cst);
            m_wakeSignal.notify_one();
        } else {
            m_wakeSignal.fetch_add(count, std::memory_order_seq_cst);
            if (count >= m_workerCount) {
                m_wakeSignal.notify_all();
            } else {
                for (u32 i = 0; i < count; ++i) {
                    m_wakeSignal.notify_one();
                }
            }
        }
    }
}

void TaskScheduler::pushMainThreadTask(TaskNode* task) noexcept {
    TaskNode* oldHead = m_mainThreadHead.load(std::memory_order_relaxed);
    do {
        task->next = oldHead;
    } while (!m_mainThreadHead.compare_exchange_weak(
        oldHead, task, std::memory_order_release, std::memory_order_relaxed));
    m_mainThreadCount.fetch_add(1, std::memory_order_relaxed);
}

Status TaskScheduler::runAndWait(TaskGraph& graph) noexcept {
    m_timelineReactor.resetTimeout();
    graph.prepareRun(*this);
    Status s = helpUntil([&graph]() noexcept {
        return graph.isCompleted();
    });
    resetWorkerArenas();
    m_timelineReactor.reset();
    return s;
}

SlabArena* TaskScheduler::currentWorkerArena() noexcept {
    u32 idx = currentWorkerIndex();
    if (idx != INVALID_WORKER_INDEX && idx < m_workerCount) {
        return &m_workers[idx].arena;
    }
    return nullptr;
}

void TaskScheduler::resetWorkerArenas() noexcept {
    for (u32 i = 0; i < m_workerCount; ++i) {
        m_workers[i].arena.reset();
    }
}

} // namespace lf
