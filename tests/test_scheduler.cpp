#include <catch2/catch_test_macros.hpp>
#include <lightflow/lightflow.hpp>
#include <lightflow/scheduler/chase_lev_deque.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <numeric>
#include <thread>
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
