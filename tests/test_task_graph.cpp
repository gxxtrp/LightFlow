#include <catch2/catch_test_macros.hpp>
#include <lightflow/lightflow.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace lf;

// =============================================================================
// TaskNode Layout Invariants
// =============================================================================

TEST_CASE("TaskNode sizing and mechanical sympathy invariants", "[graph][invariants]") {
    REQUIRE(sizeof(TaskNode) == 128);
    REQUIRE(alignof(TaskNode) == 64);
    REQUIRE(offsetof(TaskNode, inlineStorage) == 64);
}

// =============================================================================
// Linear Dependency Chains
// =============================================================================

TEST_CASE("TaskGraph linear dependency chain execution", "[graph][linear]") {
    SchedulerConfig config{.workerCount = 4};
    TaskScheduler scheduler(config);

    TaskGraph graph;
    std::atomic<u32> sequence{0};

    auto a = graph.emplace("TaskA", [&]() noexcept {
        CHECK(sequence.fetch_add(1, std::memory_order_acq_rel) == 0);
    });

    auto b = graph.emplace("TaskB", [&]() noexcept {
        CHECK(sequence.fetch_add(1, std::memory_order_acq_rel) == 1);
    });

    auto c = graph.emplace("TaskC", [&]() noexcept {
        CHECK(sequence.fetch_add(1, std::memory_order_acq_rel) == 2);
    });

    auto d = graph.emplace("TaskD", [&]() noexcept {
        CHECK(sequence.fetch_add(1, std::memory_order_acq_rel) == 3);
    });

    // Fluent chaining
    a >> b >> c >> d;

    REQUIRE(graph.nodeCount() == 4);
    scheduler.runAndWait(graph);

    CHECK(graph.isCompleted());
    CHECK(sequence.load(std::memory_order_acquire) == 4);
}

// =============================================================================
// Diamond DAG Topology
// =============================================================================

TEST_CASE("TaskGraph diamond DAG execution", "[graph][diamond]") {
    SchedulerConfig config{.workerCount = 4};
    TaskScheduler scheduler(config);

    TaskGraph graph;

    std::atomic<bool> aDone{false};
    std::atomic<bool> bDone{false};
    std::atomic<bool> cDone{false};
    std::atomic<bool> dDone{false};
    std::atomic<bool> bSawA{false};
    std::atomic<bool> cSawA{false};
    std::atomic<bool> dSawB{false};
    std::atomic<bool> dSawC{false};

    auto a = graph.emplace("A", [&]() noexcept {
        aDone.store(true, std::memory_order_release);
    });

    auto b = graph.emplace("B", [&]() noexcept {
        bSawA.store(aDone.load(std::memory_order_acquire), std::memory_order_relaxed);
        bDone.store(true, std::memory_order_release);
    });

    auto c = graph.emplace("C", [&]() noexcept {
        cSawA.store(aDone.load(std::memory_order_acquire), std::memory_order_relaxed);
        cDone.store(true, std::memory_order_release);
    });

    auto d = graph.emplace("D", [&]() noexcept {
        dSawB.store(bDone.load(std::memory_order_acquire), std::memory_order_relaxed);
        dSawC.store(cDone.load(std::memory_order_acquire), std::memory_order_relaxed);
        dDone.store(true, std::memory_order_release);
    });

    // Fan-out A -> (B, C) -> D
    a >> std::tie(b, c);
    std::tie(b, c) >> d;

    scheduler.runAndWait(graph);

    CHECK(aDone.load(std::memory_order_acquire));
    CHECK(bDone.load(std::memory_order_acquire));
    CHECK(cDone.load(std::memory_order_acquire));
    CHECK(dDone.load(std::memory_order_acquire));
    CHECK(bSawA.load(std::memory_order_relaxed));
    CHECK(cSawA.load(std::memory_order_relaxed));
    CHECK(dSawB.load(std::memory_order_relaxed));
    CHECK(dSawC.load(std::memory_order_relaxed));
    CHECK(graph.isCompleted());
}

// =============================================================================
// Wide Fan-Out and Fan-In Barrier
// =============================================================================

TEST_CASE("TaskGraph wide fan-out and fan-in barrier", "[graph][fanout]") {
    SchedulerConfig config{.workerCount = 4};
    TaskScheduler scheduler(config);

    TaskGraph graph;
    constexpr usize WIDE_COUNT = 1000;
    std::atomic<usize> completedCount{0};
    std::atomic<bool> rootDone{false};
    std::atomic<bool> allSawRoot{true};

    auto root = graph.emplace("Root", [&]() noexcept {
        rootDone.store(true, std::memory_order_release);
    });

    std::vector<TaskHandle> parallelTasks;
    parallelTasks.reserve(WIDE_COUNT);

    for (usize i = 0; i < WIDE_COUNT; ++i) {
        auto child = graph.emplace([&]() noexcept {
            if (!rootDone.load(std::memory_order_acquire)) {
                allSawRoot.store(false, std::memory_order_relaxed);
            }
            completedCount.fetch_add(1, std::memory_order_relaxed);
        });
        root.precede(child);
        parallelTasks.push_back(child);
    }

    auto barrier = graph.emplace("Barrier", [&]() noexcept {
        completedCount.fetch_add(1, std::memory_order_relaxed);
    });

    for (auto& task : parallelTasks) {
        task.precede(barrier);
    }

    scheduler.runAndWait(graph);

    CHECK(allSawRoot.load(std::memory_order_relaxed));
    CHECK(completedCount.load(std::memory_order_acquire) == WIDE_COUNT + 1);
    CHECK(graph.isCompleted());
}

// =============================================================================
// Dynamic Subflow Execution & Joining
// =============================================================================

TEST_CASE("Dynamic subflow DAG spawning and synchronization", "[graph][subflow]") {
    SchedulerConfig config{.workerCount = 4};
    TaskScheduler scheduler(config);

    TaskGraph graph;
    std::atomic<u32> counter{0};
    std::atomic<bool> preSubflowDone{false};
    std::atomic<bool> subflowTasksDone{false};
    std::atomic<bool> postSubflowDone{false};

    auto preTask = graph.emplace("PreTask", [&]() noexcept {
        preSubflowDone.store(true, std::memory_order_release);
        counter.fetch_add(1, std::memory_order_relaxed);
    });

    auto subflowTask = graph.emplaceSubflow("DynamicSubflow", [&](Subflow& sf) {
        REQUIRE(preSubflowDone.load(std::memory_order_acquire));

        auto s1 = sf.emplace("SubTask1", [&]() noexcept {
            counter.fetch_add(10, std::memory_order_relaxed);
        });

        auto s2 = sf.emplace("SubTask2", [&]() noexcept {
            counter.fetch_add(20, std::memory_order_relaxed);
        });

        auto s3 = sf.emplace("SubTask3", [&]() noexcept {
            subflowTasksDone.store(true, std::memory_order_release);
            counter.fetch_add(30, std::memory_order_relaxed);
        });

        s1 >> s2 >> s3;
    });

    auto postTask = graph.emplace("PostTask", [&]() noexcept {
        REQUIRE(subflowTasksDone.load(std::memory_order_acquire));
        postSubflowDone.store(true, std::memory_order_release);
        counter.fetch_add(100, std::memory_order_relaxed);
    });

    preTask >> subflowTask >> postTask;

    scheduler.runAndWait(graph);

    CHECK(graph.isCompleted());
    CHECK(preSubflowDone.load(std::memory_order_acquire));
    CHECK(subflowTasksDone.load(std::memory_order_acquire));
    CHECK(postSubflowDone.load(std::memory_order_acquire));
    CHECK(counter.load(std::memory_order_acquire) == 161);
}

// =============================================================================
// Condition Nodes & Branching Execution
// =============================================================================

TEST_CASE("Condition node dynamic branching and loop cycles", "[graph][condition]") {
    SchedulerConfig config{.workerCount = 4};
    TaskScheduler scheduler(config);

    SECTION("Binary condition branching: selects target branch") {
        TaskGraph graph;
        std::atomic<bool> branchAExecuted{false};
        std::atomic<bool> branchBExecuted{false};
        std::atomic<bool> joinExecuted{false};

        int conditionChoice = 1; // Select branch B (index 1)

        auto cond = graph.emplaceCondition("Condition", [&]() noexcept -> int {
            return conditionChoice;
        });

        auto branchA = graph.emplace("BranchA", [&]() noexcept {
            branchAExecuted.store(true, std::memory_order_relaxed);
        });

        auto branchB = graph.emplace("BranchB", [&]() noexcept {
            branchBExecuted.store(true, std::memory_order_relaxed);
        });

        auto join = graph.emplace("Join", [&]() noexcept {
            joinExecuted.store(true, std::memory_order_relaxed);
        });

        cond.to(0) >> branchA >> join;
        cond.to(1) >> branchB >> join;

        scheduler.runAndWait(graph);

        CHECK_FALSE(branchAExecuted.load(std::memory_order_relaxed));
        CHECK(branchBExecuted.load(std::memory_order_relaxed));
        CHECK(joinExecuted.load(std::memory_order_relaxed));
        CHECK(branchA.node()->state.load() == TaskState::Skipped);
        CHECK(branchB.node()->state.load() == TaskState::Completed);
        CHECK(join.node()->state.load() == TaskState::Completed);
    }

    SECTION("Multi-way condition branching with cascading skip") {
        TaskGraph graph;
        std::atomic<int> executedBranch{-1};
        std::atomic<bool> joinExecuted{false};

        int chosenBranch = 2; // Select branch 2 out of 4 (0, 1, 2, 3)

        auto cond = graph.emplaceCondition("MultiCond", [&]() noexcept -> int {
            return chosenBranch;
        });

        std::array<TaskHandle, 4> branches;
        for (int i = 0; i < 4; ++i) {
            branches[static_cast<usize>(i)] = graph.emplace([&executedBranch, i]() noexcept {
                executedBranch.store(i, std::memory_order_relaxed);
            });
        }

        auto join = graph.emplace("Join", [&]() noexcept {
            joinExecuted.store(true, std::memory_order_relaxed);
        });

        for (int i = 0; i < 4; ++i) {
            cond.to(i) >> branches[static_cast<usize>(i)] >> join;
        }

        scheduler.runAndWait(graph);

        CHECK(executedBranch.load(std::memory_order_relaxed) == 2);
        CHECK(joinExecuted.load(std::memory_order_relaxed));
        for (int i = 0; i < 4; ++i) {
            if (i == 2) {
                CHECK(branches[static_cast<usize>(i)].node()->state.load() == TaskState::Completed);
            } else {
                CHECK(branches[static_cast<usize>(i)].node()->state.load() == TaskState::Skipped);
            }
        }
        CHECK(join.node()->state.load() == TaskState::Completed);
        CHECK(graph.isCompleted());
    }
}

// =============================================================================
// Graph Clearing and Inline Dual-State Repeat Execution
// =============================================================================

TEST_CASE("TaskGraph clear and repeat execution invariants", "[graph][lifecycle]") {
    SchedulerConfig config{.workerCount = 4};
    TaskScheduler scheduler(config);

    TaskGraph graph;
    std::atomic<u32> count{0};

    auto t1 = graph.emplace([&count]() noexcept {
        count.fetch_add(1, std::memory_order_relaxed);
    });
    auto t2 = graph.emplace([&count]() noexcept {
        count.fetch_add(1, std::memory_order_relaxed);
    });
    t1 >> t2;

    // Run 1
    scheduler.runAndWait(graph);
    CHECK(count.load(std::memory_order_relaxed) == 2);
    CHECK(graph.isCompleted());

    // Repeat Run 2 (Inline Dual-State)
    scheduler.runAndWait(graph);
    CHECK(count.load(std::memory_order_relaxed) == 4);
    CHECK(graph.isCompleted());

    // Clear and rebuild new topology
    graph.clear();
    CHECK(graph.nodeCount() == 0);
    CHECK(graph.isCompleted()); // Empty graph has 0 pending tasks

    count.store(0, std::memory_order_relaxed);
    graph.emplace([&count]() noexcept {
        count.fetch_add(10, std::memory_order_relaxed);
    });
    scheduler.runAndWait(graph);
    CHECK(count.load(std::memory_order_relaxed) == 10);
    CHECK(graph.isCompleted());
}
