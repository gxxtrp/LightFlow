#include <catch2/catch_test_macros.hpp>
#include <lightflow/lightflow.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace lf;

// =============================================================================
// 100,000 Parallel Tasks Contention Stress
// =============================================================================

TEST_CASE("High-contention stress test: 100,000 parallel tasks across all hardware threads", "[stress][concurrency]") {
    const u32 workerThreads = std::max<u32>(4, static_cast<u32>(std::thread::hardware_concurrency()));
    SchedulerConfig config{.workerCount = workerThreads, .initialDequeCapacity = 8192};
    TaskScheduler scheduler(config);

    TaskGraph graph;
    constexpr usize TASK_COUNT = 100000;
    constexpr usize BATCH_SIZE = 10000;
    constexpr usize NUM_BATCHES = TASK_COUNT / BATCH_SIZE;
    std::atomic<usize> completedCount{0};

    auto root = graph.emplace("Root", [&]() noexcept {
        completedCount.fetch_add(1, std::memory_order_relaxed);
    });

    auto join = graph.emplace("Join", [&]() noexcept {
        completedCount.fetch_add(1, std::memory_order_relaxed);
    });

    std::vector<TaskHandle> collectors;
    collectors.reserve(NUM_BATCHES);
    for (usize b = 0; b < NUM_BATCHES; ++b) {
        auto collector = graph.emplace([&]() noexcept {
            completedCount.fetch_add(1, std::memory_order_relaxed);
        });
        collector.precede(join);
        collectors.push_back(collector);
    }

    for (usize i = 0; i < TASK_COUNT; ++i) {
        auto child = graph.emplace([&]() noexcept {
            completedCount.fetch_add(1, std::memory_order_relaxed);
        });
        root.precede(child);
        child.precede(collectors[i / BATCH_SIZE]);
    }

    constexpr usize TOTAL_NODES = TASK_COUNT + NUM_BATCHES + 2;
    REQUIRE(graph.nodeCount() == TOTAL_NODES);

    Status status = scheduler.runAndWait(graph);
    CHECK(status == Status::Success);
    CHECK(completedCount.load(std::memory_order_acquire) == TOTAL_NODES);
    CHECK(graph.isCompleted());
}

// =============================================================================
// 100,000 Tasks Multi-Stage Ordering Invariant
// =============================================================================

TEST_CASE("High-contention stress test: 100,000 tasks in multi-stage DAG with dependency ordering", "[stress][ordering]") {
    const u32 workerThreads = std::max<u32>(4, static_cast<u32>(std::thread::hardware_concurrency()));
    SchedulerConfig config{.workerCount = workerThreads, .initialDequeCapacity = 8192};
    TaskScheduler scheduler(config);

    TaskGraph graph;
    constexpr usize NUM_STAGES = 50;
    constexpr usize TASKS_PER_STAGE = 2000; // 50 * 2,000 = 100,000 tasks
    constexpr usize TOTAL_TASKS = NUM_STAGES * TASKS_PER_STAGE;

    std::atomic<usize> completedCount{0};
    std::atomic<bool> orderingViolated{false};

    std::vector<std::vector<std::atomic<bool>>> taskFinished(NUM_STAGES);
    for (auto& row : taskFinished) {
        row = std::vector<std::atomic<bool>>(TASKS_PER_STAGE);
        for (auto& f : row) {
            f.store(false, std::memory_order_relaxed);
        }
    }

    std::vector<TaskHandle> prevStage;
    prevStage.reserve(TASKS_PER_STAGE);

    for (usize s = 0; s < NUM_STAGES; ++s) {
        std::vector<TaskHandle> currStage;
        currStage.reserve(TASKS_PER_STAGE);

        TaskHandle barrier;
        if (s > 0) {
            barrier = graph.emplace([&orderingViolated, &taskFinished, s]() noexcept {
                for (usize prevIdx = 0; prevIdx < TASKS_PER_STAGE; ++prevIdx) {
                    if (!taskFinished[s - 1][prevIdx].load(std::memory_order_acquire)) {
                        orderingViolated.store(true, std::memory_order_release);
                    }
                }
            });

            for (auto& prevTask : prevStage) {
                prevTask.precede(barrier);
            }
        }

        for (usize t = 0; t < TASKS_PER_STAGE; ++t) {
            auto task = graph.emplace([&completedCount, &taskFinished, s, t]() noexcept {
                taskFinished[s][t].store(true, std::memory_order_release);
                completedCount.fetch_add(1, std::memory_order_relaxed);
            });

            if (s > 0) {
                barrier.precede(task);
            }
            currStage.push_back(task);
        }

        prevStage = std::move(currStage);
    }

    Status status = scheduler.runAndWait(graph);
    CHECK(status == Status::Success);
    CHECK(completedCount.load(std::memory_order_acquire) == TOTAL_TASKS);
    CHECK_FALSE(orderingViolated.load(std::memory_order_acquire));
}

// =============================================================================
// Multi-Producer External Submission Stress
// =============================================================================

TEST_CASE("High-contention stress test: Multi-producer concurrent external task submissions", "[stress][external]") {
    const u32 workerThreads = std::max<u32>(4, static_cast<u32>(std::thread::hardware_concurrency()));
    SchedulerConfig config{.workerCount = workerThreads, .initialDequeCapacity = 8192};
    TaskScheduler scheduler(config);

    constexpr usize PRODUCER_COUNT = 8;
    constexpr usize TASKS_PER_PRODUCER = 12500; // 8 * 12,500 = 100,000 tasks
    constexpr usize TOTAL_TASKS = PRODUCER_COUNT * TASKS_PER_PRODUCER;

    std::atomic<usize> executedCount{0};

    std::vector<SlabArena> arenas;
    arenas.reserve(PRODUCER_COUNT);
    for (usize p = 0; p < PRODUCER_COUNT; ++p) {
        arenas.emplace_back(BlockPool::global());
    }

    std::atomic<bool> startSignal{false};
    std::vector<std::thread> producers;
    producers.reserve(PRODUCER_COUNT);

    for (usize p = 0; p < PRODUCER_COUNT; ++p) {
        producers.emplace_back([&, p]() {
            while (!startSignal.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            auto& arena = arenas[p];
            for (usize i = 0; i < TASKS_PER_PRODUCER; ++i) {
                TaskNode* node = arena.create<TaskNode>();
                node->domain = TaskDomain::Worker;
                node->priority = (i % 2 == 0) ? TaskPriority::Normal : TaskPriority::High;
                node->initialInDegree = 0;
                node->inDegree.store(0, std::memory_order_relaxed);
                node->state.store(TaskState::Pending, std::memory_order_relaxed);
                node->setCallable([&executedCount]() noexcept {
                    executedCount.fetch_add(1, std::memory_order_release);
                }, &arena);

                scheduler.schedule(node);
            }
        });
    }

    startSignal.store(true, std::memory_order_release);

    for (auto& t : producers) {
        t.join();
    }

    scheduler.helpUntil([&executedCount]() noexcept {
        return executedCount.load(std::memory_order_acquire) == TOTAL_TASKS;
    });

    CHECK(executedCount.load(std::memory_order_acquire) == TOTAL_TASKS);
}
