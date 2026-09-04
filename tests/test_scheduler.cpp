#include <catch2/catch_test_macros.hpp>
#include <lightflow/lightflow.hpp>
#include <lightflow/scheduler/chase_lev_deque.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <numeric>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace lf;

// =============================================================================
// Chase-Lev Work-Stealing Deque Invariants
// =============================================================================

TEST_CASE("ChaseLevDeque single-threaded LIFO and dynamic resize", "[scheduler][deque]") {
    ChaseLevDeque deque(16);

    REQUIRE(deque.empty());
    REQUIRE(deque.size() == 0);
    REQUIRE(deque.popBottom() == nullptr);

    TaskNode n1{1, TaskPriority::Normal};
    TaskNode n2{2, TaskPriority::Normal};
    TaskNode n3{3, TaskPriority::Normal};

    deque.pushBottom(&n1);
    deque.pushBottom(&n2);
    deque.pushBottom(&n3);

    REQUIRE(deque.size() == 3);

    // LIFO popBottom
    TaskNode* p3 = deque.popBottom();
    REQUIRE(p3 != nullptr);
    CHECK(p3->id == 3);

    TaskNode* p2 = deque.popBottom();
    REQUIRE(p2 != nullptr);
    CHECK(p2->id == 2);

    TaskNode* p1 = deque.popBottom();
    REQUIRE(p1 != nullptr);
    CHECK(p1->id == 1);

    CHECK(deque.empty());
    CHECK(deque.popBottom() == nullptr);

    // Dynamic buffer growth
    constexpr usize NUM_TASKS = 256;
    std::vector<TaskNode> tasks;
    tasks.reserve(NUM_TASKS);
    for (u32 i = 0; i < NUM_TASKS; ++i) {
        tasks.push_back(TaskNode{i + 1, TaskPriority::Normal});
    }

    for (usize i = 0; i < NUM_TASKS; ++i) {
        deque.pushBottom(&tasks[i]);
    }

    CHECK(deque.size() == NUM_TASKS);
    CHECK(deque.capacity() >= NUM_TASKS);

    for (usize i = 0; i < NUM_TASKS; ++i) {
        TaskNode* popped = deque.popBottom();
        REQUIRE(popped != nullptr);
        CHECK(popped->id == tasks[NUM_TASKS - 1 - i].id);
    }
    CHECK(deque.empty());
}

TEST_CASE("ChaseLevDeque concurrent stealTop (FIFO)", "[scheduler][deque]") {
    ChaseLevDeque deque(256);
    constexpr usize NUM_ITEMS = 500;
    std::vector<TaskNode> tasks;
    tasks.reserve(NUM_ITEMS);

    for (u32 i = 0; i < NUM_ITEMS; ++i) {
        tasks.push_back(TaskNode{i + 1, TaskPriority::Normal});
        deque.pushBottom(&tasks.back());
    }

    std::atomic<bool> startSteal{false};
    std::atomic<usize> totalStolen{0};
    constexpr usize THREAD_COUNT = 4;
    std::vector<std::thread> thieves;
    thieves.reserve(THREAD_COUNT);

    for (usize t = 0; t < THREAD_COUNT; ++t) {
        thieves.emplace_back([&]() {
            while (!startSteal.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            while (true) {
                TaskNode* node = deque.stealTop();
                if (node == nullptr) {
                    if (deque.empty()) {
                        break;
                    }
                    std::this_thread::yield();
                    continue;
                }
                totalStolen.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    startSteal.store(true, std::memory_order_release);

    usize ownerPopped = 0;
    while (!deque.empty()) {
        TaskNode* node = deque.popBottom();
        if (node != nullptr) {
            ownerPopped++;
        }
    }

    for (auto& thief : thieves) {
        thief.join();
    }

    CHECK(ownerPopped + totalStolen.load(std::memory_order_acquire) == NUM_ITEMS);
}

// =============================================================================
// TaskScheduler Lifecycle and Thread Identification
// =============================================================================

TEST_CASE("TaskScheduler lifecycle and worker thread identification", "[scheduler][lifecycle]") {
    SchedulerConfig config{
        .workerCount = 4,
        .threadNamePrefix = "Worker",
        .ioWorkerCount = 1
    };
    TaskScheduler scheduler(config);

    CHECK(scheduler.workerCount() == 4);
    CHECK(scheduler.ioWorkerCount() == 1);
    CHECK_FALSE(scheduler.isShutdown());
    CHECK_FALSE(scheduler.isWorkerThread());

    std::array<std::atomic<bool>, 4> seenWorker{};
    std::array<TaskNode, 4> tasks{};

    for (u32 i = 0; i < 4; ++i) {
        tasks[i].id = i + 1;
        tasks[i].userData = &seenWorker;
        tasks[i].executeFn = [](TaskNode* node) noexcept {
            auto* seenArr = static_cast<std::array<std::atomic<bool>, 4>*>(node->userData);
            u32 idx = node->id - 1;
            (*seenArr)[idx].store(true, std::memory_order_release);
        };
        scheduler.schedule(&tasks[i]);
    }

    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(2)) {
        bool allDone = true;
        for (u32 i = 0; i < 4; ++i) {
            if (!seenWorker[i].load(std::memory_order_acquire)) {
                allDone = false;
                break;
            }
        }
        if (allDone) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    for (u32 i = 0; i < 4; ++i) {
        CHECK(seenWorker[i].load(std::memory_order_acquire));
    }
}

// =============================================================================
// Two-Tier Adaptive Spin + Futex Parking
// =============================================================================

TEST_CASE("TaskScheduler two-tier adaptive parking and futex unparking", "[scheduler][parking]") {
    SchedulerConfig config{
        .workerCount = 4,
        .ioWorkerCount = 1
    };
    TaskScheduler scheduler(config);

    // Give idle workers time to enter Tier 2 futex parking
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    CHECK(scheduler.sleepingWorkerCount() == 4);

    // Scheduling a task must wake a parked worker via futex
    std::atomic<bool> executed{false};
    TaskNode task{
        .id = 100,
        .priority = TaskPriority::Normal,
        .domain = TaskDomain::Worker,
        .userData = &executed,
        .executeFn = [](TaskNode* node) noexcept {
            auto* flag = static_cast<std::atomic<bool>*>(node->userData);
            flag->store(true, std::memory_order_release);
        }
    };

    scheduler.schedule(&task);

    auto start = std::chrono::steady_clock::now();
    while (!executed.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(2)) {
        std::this_thread::yield();
    }

    CHECK(executed.load(std::memory_order_acquire));
}

// =============================================================================
// Multi-Producer External Task Submission
// =============================================================================

TEST_CASE("TaskScheduler multi-producer concurrent external submissions", "[scheduler][dispatch]") {
    SchedulerConfig config{.workerCount = 4};
    TaskScheduler scheduler(config);

    constexpr usize NUM_PRODUCERS = 4;
    constexpr usize TASKS_PER_PRODUCER = 500;
    constexpr usize TOTAL_TASKS = NUM_PRODUCERS * TASKS_PER_PRODUCER;

    std::atomic<usize> completedCount{0};
    std::vector<TaskNode> taskNodes(TOTAL_TASKS);

    for (usize i = 0; i < TOTAL_TASKS; ++i) {
        taskNodes[i].id = static_cast<u32>(i + 1);
        taskNodes[i].userData = &completedCount;
        taskNodes[i].executeFn = [](TaskNode* node) noexcept {
            auto* cnt = static_cast<std::atomic<usize>*>(node->userData);
            cnt->fetch_add(1, std::memory_order_relaxed);
        };
    }

    std::vector<std::thread> producers;
    producers.reserve(NUM_PRODUCERS);

    for (usize p = 0; p < NUM_PRODUCERS; ++p) {
        producers.emplace_back([&, p]() {
            usize start = p * TASKS_PER_PRODUCER;
            for (usize i = 0; i < TASKS_PER_PRODUCER; ++i) {
                scheduler.schedule(&taskNodes[start + i]);
            }
        });
    }

    for (auto& producer : producers) {
        producer.join();
    }

    auto startTime = std::chrono::steady_clock::now();
    while (completedCount.load(std::memory_order_acquire) < TOTAL_TASKS &&
           std::chrono::steady_clock::now() - startTime < std::chrono::seconds(3)) {
        std::this_thread::yield();
    }

    CHECK(completedCount.load(std::memory_order_acquire) == TOTAL_TASKS);
}

// =============================================================================
// Parallel For Data Parallel Execution
// =============================================================================

TEST_CASE("ParallelFor iteration completeness, chunking, and remainder handling", "[scheduler][parallel_for]") {
    SchedulerConfig config{.workerCount = 4};
    TaskScheduler scheduler(config);

    SECTION("Even chunking: 1,024 elements with batchSize 64") {
        TaskGraph graph;
        constexpr usize COUNT = 1024;
        constexpr usize BATCH_SIZE = 64;
        std::vector<u32> data(COUNT, 0);

        graph.parallelFor(COUNT, BATCH_SIZE, [&](size_t i) noexcept {
            data[i] = static_cast<u32>(i * 5 + 2);
        });

        scheduler.runAndWait(graph);

        for (usize i = 0; i < COUNT; ++i) {
            REQUIRE(data[i] == static_cast<u32>(i * 5 + 2));
        }
    }

    SECTION("Uneven chunking: 1,000 elements with batchSize 64 (remainder chunk)") {
        TaskGraph graph;
        constexpr usize COUNT = 1000;
        constexpr usize BATCH_SIZE = 64;
        std::vector<std::atomic<u32>> visited(COUNT);
        for (auto& v : visited) {
            v.store(0, std::memory_order_relaxed);
        }

        graph.parallelFor(COUNT, BATCH_SIZE, [&](size_t i) noexcept {
            visited[i].fetch_add(1, std::memory_order_relaxed);
        });

        scheduler.runAndWait(graph);

        for (usize i = 0; i < COUNT; ++i) {
            REQUIRE(visited[i].load(std::memory_order_relaxed) == 1);
        }
    }

    SECTION("Edge case: 1 element") {
        TaskGraph graph;
        std::atomic<u32> singleVisited{0};

        graph.parallelFor(1, 64, [&](size_t i) noexcept {
            CHECK(i == 0);
            singleVisited.fetch_add(1, std::memory_order_relaxed);
        });

        scheduler.runAndWait(graph);
        CHECK(singleVisited.load(std::memory_order_relaxed) == 1);
    }
}

// =============================================================================
// TaskDomain::MainThread Execution Domain
// =============================================================================

TEST_CASE("TaskScheduler MainThread direct scheduling, count tracking, and caller-thread drain", "[scheduler][main_thread]") {
    SchedulerConfig config{
        .workerCount = 2,
        .ioWorkerCount = 1
    };
    TaskScheduler scheduler(config);

    const std::thread::id callerThreadId = std::this_thread::get_id();

    // 1. Empty queue drain invariant and idempotence
    CHECK(scheduler.mainThreadTaskCount() == 0);
    CHECK(scheduler.drainMainThreadTasks() == 0);
    CHECK(scheduler.mainThreadTaskCount() == 0);

    // 2. Direct queue & drain execution on caller thread
    struct DirectDrainContext {
        std::atomic<bool> executed{false};
        std::thread::id threadId{};
        u32 workerIndex{0};
        bool isWorker{true};
        const TaskScheduler* sched{nullptr};
    };

    DirectDrainContext ctx{};
    ctx.sched = &scheduler;

    TaskNode task{
        .id = 1,
        .priority = TaskPriority::Normal,
        .domain = TaskDomain::MainThread,
        .userData = &ctx,
        .executeFn = [](TaskNode* node) noexcept {
            auto* c = static_cast<DirectDrainContext*>(node->userData);
            c->threadId = std::this_thread::get_id();
            c->workerIndex = c->sched->currentWorkerIndex();
            c->isWorker = c->sched->isWorkerThread();
            c->executed.store(true, std::memory_order_release);
        }
    };

    scheduler.schedule(&task, TaskDomain::MainThread);

    CHECK(scheduler.mainThreadTaskCount() == 1);
    CHECK_FALSE(ctx.executed.load(std::memory_order_acquire));

    usize drained = scheduler.drainMainThreadTasks();
    CHECK(drained == 1);
    CHECK(scheduler.mainThreadTaskCount() == 0);
    CHECK(ctx.executed.load(std::memory_order_acquire));
    CHECK(ctx.threadId == callerThreadId);
    CHECK(ctx.workerIndex == TaskScheduler::INVALID_WORKER_INDEX);
    CHECK_FALSE(ctx.isWorker);

    // Draining again returns 0
    CHECK(scheduler.drainMainThreadTasks() == 0);
    CHECK(scheduler.mainThreadTaskCount() == 0);
}

TEST_CASE("TaskScheduler MainThread preserves strict FIFO submission order upon draining", "[scheduler][main_thread]") {
    SchedulerConfig config{
        .workerCount = 2,
        .ioWorkerCount = 1
    };
    TaskScheduler scheduler(config);

    constexpr usize TASK_COUNT = 32;
    std::vector<u32> executionOrder;
    executionOrder.reserve(TASK_COUNT);

    struct FifoItemContext {
        std::vector<u32>* log{nullptr};
        u32 index{0};
    };

    std::array<FifoItemContext, TASK_COUNT> contexts{};
    std::array<TaskNode, TASK_COUNT> tasks{};

    for (u32 i = 0; i < TASK_COUNT; ++i) {
        contexts[i] = FifoItemContext{&executionOrder, i};
        tasks[i] = TaskNode{
            .id = i + 1,
            .priority = TaskPriority::Normal,
            .domain = TaskDomain::MainThread,
            .userData = &contexts[i],
            .executeFn = [](TaskNode* node) noexcept {
                auto* c = static_cast<FifoItemContext*>(node->userData);
                c->log->push_back(c->index);
            }
        };
        scheduler.schedule(&tasks[i]);
    }

    CHECK(scheduler.mainThreadTaskCount() == TASK_COUNT);

    usize drained = scheduler.drainMainThreadTasks();
    CHECK(drained == TASK_COUNT);
    CHECK(scheduler.mainThreadTaskCount() == 0);

    REQUIRE(executionOrder.size() == TASK_COUNT);
    for (u32 i = 0; i < TASK_COUNT; ++i) {
        CHECK(executionOrder[i] == i);
    }
}

TEST_CASE("TaskScheduler MainThread integration with TaskGraph pipeline", "[scheduler][main_thread]") {
    SchedulerConfig config{
        .workerCount = 4,
        .ioWorkerCount = 1
    };
    TaskScheduler scheduler(config);

    const std::thread::id callerThreadId = std::this_thread::get_id();

    TaskGraph graph;
    std::atomic<bool> workerPreDone{false};
    std::atomic<bool> mainDone{false};
    std::atomic<bool> workerSuccDone{false};
    std::atomic<std::thread::id> mainExecutedThreadId{};

    auto workerPre = graph.emplace("WorkerPredecessor", TaskDomain::Worker, [&]() noexcept {
        CHECK_FALSE(mainDone.load(std::memory_order_relaxed));
        CHECK_FALSE(workerSuccDone.load(std::memory_order_relaxed));
        workerPreDone.store(true, std::memory_order_release);
    });

    auto mainTask = graph.emplace("MainThreadPass", TaskDomain::MainThread, [&]() noexcept {
        CHECK(workerPreDone.load(std::memory_order_acquire));
        CHECK_FALSE(workerSuccDone.load(std::memory_order_relaxed));
        mainExecutedThreadId.store(std::this_thread::get_id(), std::memory_order_release);
        mainDone.store(true, std::memory_order_release);
    });

    auto workerSucc = graph.emplace("WorkerSuccessor", TaskDomain::Worker, [&]() noexcept {
        CHECK(workerPreDone.load(std::memory_order_acquire));
        CHECK(mainDone.load(std::memory_order_acquire));
        workerSuccDone.store(true, std::memory_order_release);
    });

    workerPre.precede(mainTask);
    mainTask.precede(workerSucc);

    // Launch graph execution on a dedicated coordinator thread
    std::atomic<bool> runnerCompleted{false};
    std::thread runner([&]() {
        scheduler.runAndWait(graph);
        runnerCompleted.store(true, std::memory_order_release);
    });

    // Wait until workerPre finishes and mainTask is enqueued in the scheduler's main-thread queue
    auto start = std::chrono::steady_clock::now();
    while (scheduler.mainThreadTaskCount() == 0 &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(2)) {
        std::this_thread::yield();
    }

    CHECK(workerPreDone.load(std::memory_order_acquire));
    CHECK_FALSE(mainDone.load(std::memory_order_acquire));
    CHECK_FALSE(workerSuccDone.load(std::memory_order_acquire));
    CHECK_FALSE(runnerCompleted.load(std::memory_order_acquire));
    CHECK(scheduler.mainThreadTaskCount() == 1);

    // Drain main thread tasks from the caller thread
    usize drained = scheduler.drainMainThreadTasks();
    CHECK(drained == 1);
    CHECK(scheduler.mainThreadTaskCount() == 0);
    CHECK(mainDone.load(std::memory_order_acquire));
    CHECK(mainExecutedThreadId.load(std::memory_order_acquire) == callerThreadId);

    // Join runner thread (which unblocks once workerSucc completes)
    runner.join();

    CHECK(workerSuccDone.load(std::memory_order_acquire));
    CHECK(runnerCompleted.load(std::memory_order_acquire));
    CHECK(graph.isCompleted());
}

// =============================================================================
// TaskDomain::IO Background Streaming Domain
// =============================================================================

TEST_CASE("TaskScheduler IO domain tasks execute exclusively on dedicated IO worker threads", "[scheduler][io]") {
    SchedulerConfig config{
        .workerCount = 2,
        .threadNamePrefix = "Worker",
        .ioWorkerCount = 2,
        .ioThreadNamePrefix = "IO"
    };
    TaskScheduler scheduler(config);

    const std::thread::id callerThreadId = std::this_thread::get_id();

    // 1. Capture worker thread IDs
    std::array<std::atomic<std::thread::id>, 2> workerThreadIds{};
    std::array<TaskNode, 2> workerTasks{};
    for (u32 i = 0; i < 2; ++i) {
        workerTasks[i] = TaskNode{
            .id = i + 1,
            .priority = TaskPriority::Normal,
            .domain = TaskDomain::Worker,
            .userData = &workerThreadIds[i],
            .executeFn = [](TaskNode* node) noexcept {
                auto* idSlot = static_cast<std::atomic<std::thread::id>*>(node->userData);
                idSlot->store(std::this_thread::get_id(), std::memory_order_release);
            }
        };
        scheduler.schedule(&workerTasks[i]);
    }

    auto startWait = std::chrono::steady_clock::now();
    while ((workerThreadIds[0].load(std::memory_order_acquire) == std::thread::id{} ||
            workerThreadIds[1].load(std::memory_order_acquire) == std::thread::id{}) &&
           std::chrono::steady_clock::now() - startWait < std::chrono::seconds(2)) {
        std::this_thread::yield();
    }

    // 2. Schedule IO tasks and verify thread isolation
    constexpr usize IO_TASK_COUNT = 16;
    struct IoTaskRecord {
        std::thread::id threadId{};
        u32 workerIndex{0};
        bool isWorker{true};
    };

    struct IoTaskContext {
        IoTaskRecord* record{nullptr};
        const TaskScheduler* sched{nullptr};
        std::atomic<usize>* completionCounter{nullptr};
    };

    std::array<IoTaskRecord, IO_TASK_COUNT> records{};
    std::array<IoTaskContext, IO_TASK_COUNT> contexts{};
    std::array<TaskNode, IO_TASK_COUNT> ioTasks{};
    std::atomic<usize> ioCompleted{0};

    for (usize i = 0; i < IO_TASK_COUNT; ++i) {
        contexts[i] = IoTaskContext{
            .record = &records[i],
            .sched = &scheduler,
            .completionCounter = &ioCompleted
        };
        ioTasks[i] = TaskNode{
            .id = static_cast<u32>(100 + i),
            .priority = TaskPriority::Normal,
            .domain = TaskDomain::IO,
            .userData = &contexts[i],
            .executeFn = [](TaskNode* node) noexcept {
                // Sleep briefly so IO tasks overlap across both IO threads
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                auto* c = static_cast<IoTaskContext*>(node->userData);
                c->record->threadId = std::this_thread::get_id();
                c->record->workerIndex = c->sched->currentWorkerIndex();
                c->record->isWorker = c->sched->isWorkerThread();
                c->completionCounter->fetch_add(1, std::memory_order_release);
            }
        };
        scheduler.schedule(&ioTasks[i]);
    }

    auto startIo = std::chrono::steady_clock::now();
    while (ioCompleted.load(std::memory_order_acquire) < IO_TASK_COUNT &&
           std::chrono::steady_clock::now() - startIo < std::chrono::seconds(3)) {
        std::this_thread::yield();
    }

    CHECK(ioCompleted.load(std::memory_order_acquire) == IO_TASK_COUNT);

    std::unordered_set<std::thread::id> distinctIoThreads;
    for (usize i = 0; i < IO_TASK_COUNT; ++i) {
        CHECK(records[i].workerIndex == TaskScheduler::INVALID_WORKER_INDEX);
        CHECK_FALSE(records[i].isWorker);
        CHECK(records[i].threadId != callerThreadId);
        CHECK(records[i].threadId != workerThreadIds[0].load(std::memory_order_acquire));
        CHECK(records[i].threadId != workerThreadIds[1].load(std::memory_order_acquire));
        distinctIoThreads.insert(records[i].threadId);
    }

    // Both IO workers participated
    CHECK(distinctIoThreads.size() == 2);
}

TEST_CASE("TaskScheduler concurrent streaming burst: 1000 IO and 1000 Worker tasks with futex wakeups", "[scheduler][io]") {
    SchedulerConfig config{
        .workerCount = 4,
        .ioWorkerCount = 2
    };
    TaskScheduler scheduler(config);

    // Allow workers and IO threads to park in Tier 2 futex sleep
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    constexpr usize TOTAL_IO_TASKS = 1000;
    constexpr usize TOTAL_WORKER_TASKS = 1000;
    constexpr usize PRODUCER_COUNT = 4;
    constexpr usize IO_PER_PRODUCER = TOTAL_IO_TASKS / PRODUCER_COUNT;
    constexpr usize WORKER_PER_PRODUCER = TOTAL_WORKER_TASKS / PRODUCER_COUNT;

    std::atomic<usize> ioCompleted{0};
    std::atomic<usize> workerCompleted{0};

    std::vector<TaskNode> ioTasks(TOTAL_IO_TASKS);
    std::vector<TaskNode> workerTasks(TOTAL_WORKER_TASKS);

    for (usize i = 0; i < TOTAL_IO_TASKS; ++i) {
        ioTasks[i] = TaskNode{
            .id = static_cast<u32>(i + 1),
            .priority = TaskPriority::Normal,
            .domain = TaskDomain::IO,
            .userData = &ioCompleted,
            .executeFn = [](TaskNode* node) noexcept {
                auto* cnt = static_cast<std::atomic<usize>*>(node->userData);
                cnt->fetch_add(1, std::memory_order_release);
            }
        };
    }

    for (usize i = 0; i < TOTAL_WORKER_TASKS; ++i) {
        workerTasks[i] = TaskNode{
            .id = static_cast<u32>(i + 1),
            .priority = TaskPriority::Normal,
            .domain = TaskDomain::Worker,
            .userData = &workerCompleted,
            .executeFn = [](TaskNode* node) noexcept {
                auto* cnt = static_cast<std::atomic<usize>*>(node->userData);
                cnt->fetch_add(1, std::memory_order_release);
            }
        };
    }

    std::atomic<bool> startSignal{false};
    std::vector<std::thread> producers;
    producers.reserve(PRODUCER_COUNT);

    for (usize p = 0; p < PRODUCER_COUNT; ++p) {
        producers.emplace_back([&, p]() {
            while (!startSignal.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            const usize ioStart = p * IO_PER_PRODUCER;
            const usize workerStart = p * WORKER_PER_PRODUCER;

            for (usize i = 0; i < IO_PER_PRODUCER; ++i) {
                scheduler.schedule(&ioTasks[ioStart + i]);
                scheduler.schedule(&workerTasks[workerStart + i]);
            }
        });
    }

    startSignal.store(true, std::memory_order_release);

    for (auto& producer : producers) {
        producer.join();
    }

    auto start = std::chrono::steady_clock::now();
    while ((ioCompleted.load(std::memory_order_acquire) < TOTAL_IO_TASKS ||
            workerCompleted.load(std::memory_order_acquire) < TOTAL_WORKER_TASKS) &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
        std::this_thread::yield();
    }

    CHECK(ioCompleted.load(std::memory_order_acquire) == TOTAL_IO_TASKS);
    CHECK(workerCompleted.load(std::memory_order_acquire) == TOTAL_WORKER_TASKS);
}

TEST_CASE("TaskScheduler clean shutdown and joining with active and idle IO worker threads", "[scheduler][io]") {
    SECTION("Idle IO workers wake from futex and join promptly on shutdown") {
        SchedulerConfig config{
            .workerCount = 2,
            .ioWorkerCount = 4
        };
        auto scheduler = std::make_unique<TaskScheduler>(config);
        CHECK(scheduler->ioWorkerCount() == 4);

        // Sleep to ensure IO workers are parked in futex
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        auto start = std::chrono::steady_clock::now();
        scheduler->shutdown();
        auto elapsed = std::chrono::steady_clock::now() - start;

        CHECK(scheduler->isShutdown());
        CHECK(elapsed < std::chrono::milliseconds(500));

        scheduler.reset();
    }

    SECTION("Pending IO tasks drain on shutdown before threads exit") {
        SchedulerConfig config{
            .workerCount = 1,
            .ioWorkerCount = 2
        };
        auto scheduler = std::make_unique<TaskScheduler>(config);

        constexpr usize DRAIN_TASK_COUNT = 50;
        std::atomic<usize> completed{0};
        std::vector<TaskNode> tasks(DRAIN_TASK_COUNT);

        for (usize i = 0; i < DRAIN_TASK_COUNT; ++i) {
            tasks[i] = TaskNode{
                .id = static_cast<u32>(i + 1),
                .priority = TaskPriority::Normal,
                .domain = TaskDomain::IO,
                .userData = &completed,
                .executeFn = [](TaskNode* node) noexcept {
                    auto* cnt = static_cast<std::atomic<usize>*>(node->userData);
                    cnt->fetch_add(1, std::memory_order_relaxed);
                }
            };
            scheduler->schedule(&tasks[i]);
        }

        scheduler->shutdown();

        CHECK(scheduler->isShutdown());
        CHECK(completed.load(std::memory_order_acquire) == DRAIN_TASK_COUNT);

        scheduler.reset();
    }
}

TEST_CASE("DualPriorityQueue, TaskScheduler scheduleBatch, affinity, and timeline device coverage", "[scheduler][coverage]") {
    SECTION("DualPriorityQueue operations") {
        DualPriorityQueue q(64);
        CHECK(q.empty());
        CHECK(q.size() == 0);
        CHECK(q.highDeque().empty());
        CHECK(q.normalDeque().empty());

        TaskNode n1{};
        TaskNode n2{};

        // Push High priority
        q.push(&n1, TaskPriority::High);
        CHECK_FALSE(q.empty());
        CHECK(q.size() == 1);
        CHECK(q.deque(TaskPriority::High).size() == 1);
        CHECK(q.deque(TaskPriority::Normal).size() == 0);

        // Push Normal priority
        q.push(&n2, TaskPriority::Normal);
        CHECK(q.size() == 2);

        // Const access
        const DualPriorityQueue& cq = q;
        CHECK(cq.highDeque().size() == 1);
        CHECK(cq.normalDeque().size() == 1);
        CHECK(cq.deque(TaskPriority::High).size() == 1);
        CHECK(cq.deque(TaskPriority::Normal).size() == 1);

        // Steal batch
        std::array<TaskNode*, 4> dest{};
        usize stolen = q.stealBatch(dest);
        CHECK(stolen >= 1);

        // Pop remaining
        while (q.pop() != nullptr) {}
        CHECK(q.empty());
    }

    SECTION("TaskScheduler scheduleBatch, timeline device, and affinity") {
        std::array<u32, 2> affinity{0, 1};
        SchedulerConfig config{
            .workerCount = 2,
            .coreAffinity = affinity // tests set_thread_affinity
        };
        TaskScheduler scheduler(config);

        // Test workerDeque accessor
        DualPriorityQueue& dq0 = scheduler.workerDeque(0);
        CHECK(dq0.empty());

        // Test timeline device setters/getters
        struct MockDev : public ITimelineDevice {
            u64 getCompletedValue(TimelineHandle) noexcept override { return 0; }
        } dev;

        scheduler.setTimelineDevice(&dev);
        CHECK(scheduler.timelineDevice() == &dev);
        CHECK_FALSE(scheduler.timelineReactor().hasPending());

        // Test scheduleBatch with mixed domains and priorities
        std::atomic<u32> executedCount{0};
        TaskNode tWorker{};
        tWorker.domain = TaskDomain::Worker;
        tWorker.priority = TaskPriority::High;
        tWorker.executeFn = [](TaskNode* n) noexcept {
            auto* cnt = static_cast<std::atomic<u32>*>(n->userData);
            cnt->fetch_add(1, std::memory_order_relaxed);
        };
        tWorker.userData = &executedCount;

        TaskNode tMain{};
        tMain.domain = TaskDomain::MainThread;
        tMain.priority = TaskPriority::Normal;
        tMain.executeFn = [](TaskNode* n) noexcept {
            auto* cnt = static_cast<std::atomic<u32>*>(n->userData);
            cnt->fetch_add(1, std::memory_order_relaxed);
        };
        tMain.userData = &executedCount;

        std::array<TaskNode*, 3> batch{&tWorker, &tMain, nullptr};
        scheduler.scheduleBatch(batch);

        // Empty batch
        scheduler.scheduleBatch({});

        // Drain main thread
        scheduler.drainMainThreadTasks();

        // Wait with runAndWait(std::atomic<u32>&)
        std::atomic<u32> pending{1};
        std::thread finishThread([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            pending.store(0, std::memory_order_release);
        });
        scheduler.runAndWait(pending);
        finishThread.join();

        CHECK(executedCount.load(std::memory_order_relaxed) >= 1);

        scheduler.setTimelineDevice(nullptr);
        CHECK(scheduler.timelineDevice() == nullptr);
    }
}


