#include <catch2/catch_test_macros.hpp>
#include <lightflow/lightflow.hpp>
#include "test_harness.hpp"

#include <array>
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

    SECTION("3-tier deep skip cascade resolution") {
        TaskGraph graph;
        std::atomic<bool> joinExecuted{false};
        std::atomic<u32> executedCount{0};

        // Condition chooses branch 0 out of 3 branches (0, 1, 2)
        auto cond = graph.emplaceCondition("CascadeCond", []() noexcept -> int {
            return 0;
        });

        // Branch 0 (3 tiers): Diamond DAG structure
        std::atomic<bool> b0_t1{false}, b0_t2a{false}, b0_t2b{false}, b0_t3{false};
        auto n0_1 = graph.emplace("B0_T1", [&]() noexcept { b0_t1.store(true, std::memory_order_relaxed); executedCount.fetch_add(1, std::memory_order_relaxed); });
        auto n0_2a = graph.emplace("B0_T2A", [&]() noexcept { b0_t2a.store(true, std::memory_order_relaxed); executedCount.fetch_add(1, std::memory_order_relaxed); });
        auto n0_2b = graph.emplace("B0_T2B", [&]() noexcept { b0_t2b.store(true, std::memory_order_relaxed); executedCount.fetch_add(1, std::memory_order_relaxed); });
        auto n0_3 = graph.emplace("B0_T3", [&]() noexcept { b0_t3.store(true, std::memory_order_relaxed); executedCount.fetch_add(1, std::memory_order_relaxed); });
        cond.to(0) >> n0_1 >> std::tie(n0_2a, n0_2b);
        std::tie(n0_2a, n0_2b) >> n0_3;

        // Branch 1 (3 tiers): Diamond DAG structure - skipped
        std::atomic<bool> b1_t1{false}, b1_t2a{false}, b1_t2b{false}, b1_t3{false};
        auto n1_1 = graph.emplace("B1_T1", [&]() noexcept { b1_t1.store(true, std::memory_order_relaxed); executedCount.fetch_add(1, std::memory_order_relaxed); });
        auto n1_2a = graph.emplace("B1_T2A", [&]() noexcept { b1_t2a.store(true, std::memory_order_relaxed); executedCount.fetch_add(1, std::memory_order_relaxed); });
        auto n1_2b = graph.emplace("B1_T2B", [&]() noexcept { b1_t2b.store(true, std::memory_order_relaxed); executedCount.fetch_add(1, std::memory_order_relaxed); });
        auto n1_3 = graph.emplace("B1_T3", [&]() noexcept { b1_t3.store(true, std::memory_order_relaxed); executedCount.fetch_add(1, std::memory_order_relaxed); });
        cond.to(1) >> n1_1 >> std::tie(n1_2a, n1_2b);
        std::tie(n1_2a, n1_2b) >> n1_3;

        // Branch 2 (3 tiers): Diamond DAG structure - skipped
        std::atomic<bool> b2_t1{false}, b2_t2a{false}, b2_t2b{false}, b2_t3{false};
        auto n2_1 = graph.emplace("B2_T1", [&]() noexcept { b2_t1.store(true, std::memory_order_relaxed); executedCount.fetch_add(1, std::memory_order_relaxed); });
        auto n2_2a = graph.emplace("B2_T2A", [&]() noexcept { b2_t2a.store(true, std::memory_order_relaxed); executedCount.fetch_add(1, std::memory_order_relaxed); });
        auto n2_2b = graph.emplace("B2_T2B", [&]() noexcept { b2_t2b.store(true, std::memory_order_relaxed); executedCount.fetch_add(1, std::memory_order_relaxed); });
        auto n2_3 = graph.emplace("B2_T3", [&]() noexcept { b2_t3.store(true, std::memory_order_relaxed); executedCount.fetch_add(1, std::memory_order_relaxed); });
        cond.to(2) >> n2_1 >> std::tie(n2_2a, n2_2b);
        std::tie(n2_2a, n2_2b) >> n2_3;

        // Join node resolves immediately when active and skipped paths finish
        auto join = graph.emplace("CascadeJoin", [&]() noexcept {
            joinExecuted.store(true, std::memory_order_release);
        });

        std::tie(n0_3, n1_3, n2_3) >> join;

        scheduler.runAndWait(graph);

        // Branch 0 executed
        CHECK(b0_t1.load(std::memory_order_relaxed));
        CHECK(b0_t2a.load(std::memory_order_relaxed));
        CHECK(b0_t2b.load(std::memory_order_relaxed));
        CHECK(b0_t3.load(std::memory_order_relaxed));

        // Branch 1 skipped
        CHECK_FALSE(b1_t1.load(std::memory_order_relaxed));
        CHECK_FALSE(b1_t2a.load(std::memory_order_relaxed));
        CHECK_FALSE(b1_t2b.load(std::memory_order_relaxed));
        CHECK_FALSE(b1_t3.load(std::memory_order_relaxed));
        CHECK(n1_1.node()->state.load() == TaskState::Skipped);
        CHECK(n1_2a.node()->state.load() == TaskState::Skipped);
        CHECK(n1_2b.node()->state.load() == TaskState::Skipped);
        CHECK(n1_3.node()->state.load() == TaskState::Skipped);

        // Branch 2 skipped
        CHECK_FALSE(b2_t1.load(std::memory_order_relaxed));
        CHECK_FALSE(b2_t2a.load(std::memory_order_relaxed));
        CHECK_FALSE(b2_t2b.load(std::memory_order_relaxed));
        CHECK_FALSE(b2_t3.load(std::memory_order_relaxed));
        CHECK(n2_1.node()->state.load() == TaskState::Skipped);
        CHECK(n2_2a.node()->state.load() == TaskState::Skipped);
        CHECK(n2_2b.node()->state.load() == TaskState::Skipped);
        CHECK(n2_3.node()->state.load() == TaskState::Skipped);

        // Join executed immediately
        CHECK(joinExecuted.load(std::memory_order_acquire));
        CHECK(join.node()->state.load() == TaskState::Completed);
        CHECK(graph.isCompleted());
        CHECK(executedCount.load(std::memory_order_relaxed) == 4);
    }

    SECTION("Dynamic condition loop cycles terminating after N iterations") {
        TaskGraph graph;
        constexpr int TARGET_ITERATIONS = 10;
        std::atomic<int> loopCounter{0};
        std::atomic<bool> doneExecuted{false};

        auto init = graph.emplace("Init", [&]() noexcept {
            loopCounter.store(0, std::memory_order_relaxed);
        });

        auto body = graph.emplace("LoopBody", [&]() noexcept {
            loopCounter.fetch_add(1, std::memory_order_relaxed);
        });

        auto cond = graph.emplaceCondition("LoopCond", [&]() noexcept -> int {
            return (loopCounter.load(std::memory_order_relaxed) < TARGET_ITERATIONS) ? 0 : 1;
        });

        auto done = graph.emplace("Done", [&]() noexcept {
            doneExecuted.store(true, std::memory_order_release);
        });

        init >> body >> cond;
        cond.to(0) >> body; // Loop back
        cond.to(1) >> done; // Exit

        scheduler.runAndWait(graph);

        CHECK(loopCounter.load(std::memory_order_relaxed) == TARGET_ITERATIONS);
        CHECK(doneExecuted.load(std::memory_order_acquire));
        CHECK(done.node()->state.load() == TaskState::Completed);
        CHECK(graph.isCompleted());
    }

    SECTION("Out-of-bounds branch return safely cascades skips") {
        TaskGraph graph;
        std::atomic<bool> b0{false};
        std::atomic<bool> b1{false};
        std::atomic<bool> join{false};

        auto cond = graph.emplaceCondition("OOB_Cond", []() noexcept -> int {
            return 99; // Out of bounds branch
        });

        auto branch0 = graph.emplace("B0", [&]() noexcept { b0.store(true, std::memory_order_relaxed); });
        auto branch1 = graph.emplace("B1", [&]() noexcept { b1.store(true, std::memory_order_relaxed); });
        auto joinNode = graph.emplace("Join", [&]() noexcept { join.store(true, std::memory_order_relaxed); });

        cond.to(0) >> branch0 >> joinNode;
        cond.to(1) >> branch1 >> joinNode;

        scheduler.runAndWait(graph);

        CHECK_FALSE(b0.load(std::memory_order_relaxed));
        CHECK_FALSE(b1.load(std::memory_order_relaxed));
        CHECK_FALSE(join.load(std::memory_order_relaxed));
        CHECK(branch0.node()->state.load() == TaskState::Skipped);
        CHECK(branch1.node()->state.load() == TaskState::Skipped);
        CHECK(joinNode.node()->state.load() == TaskState::Skipped);
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

// =============================================================================
// ParallelFor Boundary Invariants, Chunking, and Remainder Partitioning
// =============================================================================

TEST_CASE("ParallelFor boundary invariants, chunking, and remainder partitioning", "[graph][parallel_for]") {
    SchedulerConfig config{.workerCount = 4};
    TaskScheduler scheduler(config);

    SECTION("count = 0: Completes immediately with zero lambda invocations and zero deadlocks") {
        TaskGraph graph;
        std::atomic<u32> callCount{0};
        std::atomic<bool> preDone{false};
        std::atomic<bool> postDone{false};

        auto pre = graph.emplace("Pre", [&]() noexcept {
            preDone.store(true, std::memory_order_release);
        });

        auto pfor = graph.parallelFor("EmptyLoop", 0, 64, [&](usize /*i*/) noexcept {
            callCount.fetch_add(1, std::memory_order_relaxed);
        });

        auto post = graph.emplace("Post", [&]() noexcept {
            postDone.store(true, std::memory_order_release);
        });

        pre >> pfor >> post;

        CHECK(pfor.chunkCount() == 0);
        scheduler.runAndWait(graph);

        CHECK(preDone.load(std::memory_order_acquire));
        CHECK(postDone.load(std::memory_order_acquire));
        CHECK(callCount.load(std::memory_order_relaxed) == 0);
        CHECK(graph.isCompleted());
    }

    SECTION("count = 1: Single index across batch sizes (1, 64)") {
        for (usize batchSize : {usize{1}, usize{64}}) {
            TaskGraph graph;
            std::atomic<u32> singleVisited{0};
            std::atomic<usize> processedIdx{999};

            auto pfor = graph.parallelFor("SingleItem", 1, batchSize, [&](usize i) noexcept {
                processedIdx.store(i, std::memory_order_relaxed);
                singleVisited.fetch_add(1, std::memory_order_relaxed);
            });

            CHECK(pfor.chunkCount() == 1);
            scheduler.runAndWait(graph);

            CHECK(singleVisited.load(std::memory_order_relaxed) == 1);
            CHECK(processedIdx.load(std::memory_order_relaxed) == 0);
            CHECK(graph.isCompleted());
        }
    }

    SECTION("Non-divisible range: 100 items with batchSize 7 (15 chunks total: 14x7 + 1x2)") {
        TaskGraph graph;
        constexpr usize COUNT = 100;
        constexpr usize BATCH_SIZE = 7;
        std::vector<std::atomic<u32>> visited(COUNT);
        for (auto& v : visited) {
            v.store(0, std::memory_order_relaxed);
        }

        std::atomic<bool> outOfBounds{false};
        auto pfor = graph.parallelFor("NonDivisible", COUNT, BATCH_SIZE, [&](usize i) noexcept {
            if (i >= COUNT) {
                outOfBounds.store(true, std::memory_order_relaxed);
            }
            visited[i].fetch_add(1, std::memory_order_relaxed);
        });

        CHECK(pfor.chunkCount() == 15);
        scheduler.runAndWait(graph);

        CHECK_FALSE(outOfBounds.load(std::memory_order_relaxed));
        for (usize i = 0; i < COUNT; ++i) {
            REQUIRE(visited[i].load(std::memory_order_relaxed) == 1);
        }
        CHECK(graph.isCompleted());
    }

    SECTION("batchSize >= count: Creates single chunk covering [0, count)") {
        TaskGraph graph;
        constexpr usize COUNT = 10;
        constexpr usize BATCH_SIZE = 64;
        std::atomic<u32> processedMask{0};

        auto pfor = graph.parallelFor("BatchGELen", COUNT, BATCH_SIZE, [&](usize i) noexcept {
            processedMask.fetch_or(1u << i, std::memory_order_relaxed);
        });

        CHECK(pfor.chunkCount() == 1);
        scheduler.runAndWait(graph);

        CHECK(processedMask.load(std::memory_order_relaxed) == ((1u << COUNT) - 1));
        CHECK(graph.isCompleted());
    }

    SECTION("batchSize = 0: Clamped safely to 1") {
        TaskGraph graph;
        constexpr usize COUNT = 5;
        std::atomic<u32> count{0};

        auto pfor = graph.parallelFor("ZeroBatch", COUNT, 0, [&](usize /*i*/) noexcept {
            count.fetch_add(1, std::memory_order_relaxed);
        });

        CHECK(pfor.chunkCount() == 5);
        scheduler.runAndWait(graph);

        CHECK(count.load(std::memory_order_relaxed) == 5);
        CHECK(graph.isCompleted());
    }

    SECTION("ChunkRange overload with remainder verification") {
        TaskGraph graph;
        constexpr usize COUNT = 25;
        constexpr usize BATCH_SIZE = 8;
        std::vector<std::atomic<u32>> visited(COUNT);
        for (auto& v : visited) {
            v.store(0, std::memory_order_relaxed);
        }

        std::atomic<bool> rangeExceeded{false};
        auto pfor = graph.parallelFor("RangeOverload", COUNT, BATCH_SIZE, [&](const ChunkRange& range) noexcept {
            if (range.size() > BATCH_SIZE) {
                rangeExceeded.store(true, std::memory_order_relaxed);
            }
            for (usize i = range.start; i < range.end; ++i) {
                visited[i].fetch_add(1, std::memory_order_relaxed);
            }
        });

        CHECK(pfor.chunkCount() == 4); // 8, 8, 8, 1
        scheduler.runAndWait(graph);

        CHECK_FALSE(rangeExceeded.load(std::memory_order_relaxed));
        for (usize i = 0; i < COUNT; ++i) {
            REQUIRE(visited[i].load(std::memory_order_relaxed) == 1);
        }
        CHECK(graph.isCompleted());
    }
}

// =============================================================================
// Dynamic Subflow Multi-Level Nesting & Continuation Wiring
// =============================================================================

TEST_CASE("Dynamic Subflow multi-level nesting and grandchild continuation wiring", "[graph][subflow][nesting]") {
    SchedulerConfig config{.workerCount = 4};
    TaskScheduler scheduler(config);

    TaskGraph graph;
    std::atomic<u32> executionSequence{0};
    std::atomic<u32> rootPreSeq{0};
    std::atomic<u32> parentPreSeq{0};
    std::atomic<u32> gc1Seq{0};
    std::atomic<u32> gc2Seq{0};
    std::atomic<u32> gc3Seq{0};
    std::atomic<u32> parentPostSeq{0};
    std::atomic<u32> rootPostSeq{0};

    auto rootPre = graph.emplace("RootPre", [&]() noexcept {
        rootPreSeq.store(executionSequence.fetch_add(1, std::memory_order_acq_rel), std::memory_order_relaxed);
    });

    auto parentSubflow = graph.emplaceSubflow("ParentSubflow", [&](Subflow& parentSf) {
        auto parentPre = parentSf.emplace("ParentPre", [&]() noexcept {
            parentPreSeq.store(executionSequence.fetch_add(1, std::memory_order_acq_rel), std::memory_order_relaxed);
        });

        auto childSubflow = parentSf.emplaceSubflow("ChildSubflow", [&](Subflow& childSf) {
            auto gc1 = childSf.emplace("Grandchild1", [&]() noexcept {
                gc1Seq.store(executionSequence.fetch_add(1, std::memory_order_acq_rel), std::memory_order_relaxed);
            });
            auto gc2 = childSf.emplace("Grandchild2", [&]() noexcept {
                gc2Seq.store(executionSequence.fetch_add(1, std::memory_order_acq_rel), std::memory_order_relaxed);
            });
            auto gc3 = childSf.emplace("Grandchild3", [&]() noexcept {
                gc3Seq.store(executionSequence.fetch_add(1, std::memory_order_acq_rel), std::memory_order_relaxed);
            });
            gc1 >> gc2 >> gc3;
        });

        auto parentPost = parentSf.emplace("ParentPost", [&]() noexcept {
            parentPostSeq.store(executionSequence.fetch_add(1, std::memory_order_acq_rel), std::memory_order_relaxed);
        });

        parentPre >> childSubflow >> parentPost;
    });

    auto rootPost = graph.emplace("RootPost", [&]() noexcept {
        rootPostSeq.store(executionSequence.fetch_add(1, std::memory_order_acq_rel), std::memory_order_relaxed);
    });

    rootPre >> parentSubflow >> rootPost;

    scheduler.runAndWait(graph);

    CHECK(rootPreSeq.load(std::memory_order_relaxed) == 0);
    CHECK(parentPreSeq.load(std::memory_order_relaxed) == 1);
    CHECK(gc1Seq.load(std::memory_order_relaxed) == 2);
    CHECK(gc2Seq.load(std::memory_order_relaxed) == 3);
    CHECK(gc3Seq.load(std::memory_order_relaxed) == 4);
    CHECK(parentPostSeq.load(std::memory_order_relaxed) == 5);
    CHECK(rootPostSeq.load(std::memory_order_relaxed) == 6);
    CHECK(executionSequence.load(std::memory_order_acquire) == 7);
    CHECK(graph.isCompleted());
}

// =============================================================================
// MoveOnlyTask SBO Heap Spillover & SlabArena Allocation
// =============================================================================

namespace {

struct alignas(16) LargeCapturePayload {
    std::array<u64, 16> data{}; // 128 bytes (exceeds 48-byte SBO)
    std::atomic<u32>* execCounter{nullptr};
    std::atomic<u32>* dtorCounter{nullptr};

    LargeCapturePayload(std::atomic<u32>* exec, std::atomic<u32>* dtor) noexcept
        : execCounter(exec), dtorCounter(dtor) {
        for (usize i = 0; i < data.size(); ++i) {
            data[i] = static_cast<u64>(i * 1000 + 42);
        }
    }

    ~LargeCapturePayload() noexcept {
        if (dtorCounter != nullptr) {
            dtorCounter->fetch_add(1, std::memory_order_relaxed);
        }
    }

    LargeCapturePayload(const LargeCapturePayload&) = delete;
    LargeCapturePayload& operator=(const LargeCapturePayload&) = delete;

    LargeCapturePayload(LargeCapturePayload&& other) noexcept
        : data(other.data),
          execCounter(other.execCounter),
          dtorCounter(other.dtorCounter) {
        other.dtorCounter = nullptr;
    }

    LargeCapturePayload& operator=(LargeCapturePayload&& other) noexcept {
        if (this != &other) {
            data = other.data;
            execCounter = other.execCounter;
            dtorCounter = other.dtorCounter;
            other.dtorCounter = nullptr;
        }
        return *this;
    }

    void operator()() const noexcept {
        for (usize i = 0; i < data.size(); ++i) {
            if (data[i] != static_cast<u64>(i * 1000 + 42)) {
                return;
            }
        }
        if (execCounter != nullptr) {
            execCounter->fetch_add(1, std::memory_order_relaxed);
        }
    }
};

static_assert(sizeof(LargeCapturePayload) >= 128);

} // anonymous namespace

TEST_CASE("MoveOnlyTask SBO heap spillover, SlabArena bump allocation, and destruction", "[graph][sbo][arena]") {
    STATIC_REQUIRE(sizeof(LargeCapturePayload) > MoveOnlyTask::SBO_SIZE);

    SECTION("Large capture payload emplaced in TaskGraph allocates from SlabArena with zero heap calls") {
        SchedulerConfig config{.workerCount = 2};
        TaskScheduler scheduler(config);

        TaskGraph graph;
        std::atomic<u32> execCount{0};
        std::atomic<u32> dtorCount{0};

        lf::test::TestHarnessAllocator::reset_stats();
        const usize baselineAllocations = lf::test::TestHarnessAllocator::stats().alloc_count.load(std::memory_order_relaxed);

        graph.emplace(LargeCapturePayload(&execCount, &dtorCount));

        // Emplacing the large closure should NOT trigger host virtual memory allocation
        CHECK(lf::test::TestHarnessAllocator::stats().alloc_count.load(std::memory_order_relaxed) == baselineAllocations);

        scheduler.runAndWait(graph);

        CHECK(execCount.load(std::memory_order_acquire) == 1);
        CHECK(graph.isCompleted());

        // Clearing the graph must invoke the non-trivial destructor
        graph.clear();
        CHECK(dtorCount.load(std::memory_order_acquire) == 1);
    }

    SECTION("Standalone MoveOnlyTask with SlabArena overflow storage lifecycle") {
        BlockPool pool(16);
        SlabArena arena(pool);

        std::atomic<u32> execCount{0};
        std::atomic<u32> dtorCount{0};

        {
            MoveOnlyTask task(LargeCapturePayload(&execCount, &dtorCount), &arena);
            CHECK(task.valid());

            // Move construction
            MoveOnlyTask movedTask(std::move(task));
            CHECK(movedTask.valid());
            CHECK_FALSE(task.valid());

            // Move assignment
            MoveOnlyTask assignedTask;
            assignedTask = std::move(movedTask);
            CHECK(assignedTask.valid());

            // Execution
            assignedTask();
            CHECK(execCount.load(std::memory_order_relaxed) == 1);

            // Explicit reset
            assignedTask.reset();
            CHECK_FALSE(assignedTask.valid());
            CHECK(dtorCount.load(std::memory_order_relaxed) == 1);
        }
        // Destructor should not have double-destroyed
        CHECK(dtorCount.load(std::memory_order_relaxed) == 1);
    }
}

TEST_CASE("TaskHandle, BranchHandle, ParallelForHandle ergonomics, and Library Metadata", "[graph][coverage]") {
    SECTION("Library version and feature queries") {
        CHECK_FALSE(lf::version().empty());
        CHECK_FALSE(lf::is_rtti_enabled());
        CHECK_FALSE(lf::are_exceptions_enabled());
    }

    SECTION("TaskHandle ergonomics and fluent modifiers") {
        TaskGraph graph;
        TaskHandle h1 = graph.emplace([]() noexcept {});
        TaskHandle h2 = graph.emplace([]() noexcept {});
        TaskHandle h3 = graph.emplace([]() noexcept {});

        // Booleans and node access
        CHECK(static_cast<bool>(h1));
        CHECK(h1.node() != nullptr);
        CHECK(h1.exitNode() == h1.node());
        CHECK(h1->id == h1.node()->id);
        CHECK_FALSE(h1.isCompound());
        CHECK(h1 == h1);
        CHECK_FALSE(h1 == h2);

        // Status
        CHECK(h1.status() == Status::Success);
        TaskHandle nullHandle{};
        CHECK_FALSE(static_cast<bool>(nullHandle));
        CHECK(nullHandle.status() == Status::Error);

        // Modifiers and getters
        h1.name("TestTask");
        CHECK(std::string_view(h1.name()) == "TestTask");

        h1.domain(TaskDomain::MainThread);
        CHECK(h1.domain() == TaskDomain::MainThread);

        h1.priority(TaskPriority::High);
        CHECK(h1.priority() == TaskPriority::High);

        // Succeed and precede spans
        h2.succeed(h1);
        std::array<TaskHandle, 2> targets{h2, h3};
        h1.precede(targets);
        h3.succeed(std::span<const TaskHandle>{targets.data(), 1});
    }

    SECTION("BranchHandle methods") {
        TaskGraph graph;
        ConditionHandle cond = graph.emplaceCondition([]() noexcept { return 0; });
        ConditionHandle condCopy(static_cast<TaskHandle>(cond));
        BranchHandle b0 = cond.to(0);
        CHECK(b0.node() == cond.node());
        CHECK(b0.branch() == 0);

        BranchHandle b1(cond.node(), 1);
        CHECK(b1.branch() == 1);

        TaskHandle t1 = graph.emplace([]() noexcept {});
        TaskHandle t2 = graph.emplace([]() noexcept {});
        std::array<TaskHandle, 2> targets{t1, t2};
        b0.precede(targets);
    }

    SECTION("ParallelForHandle ergonomics and ChunkRange") {
        TaskGraph graph;
        ChunkRange emptyRange{10, 5};
        CHECK(emptyRange.empty());
        ChunkRange validRange{0, 10};
        CHECK_FALSE(validRange.empty());

        ChunkIndex cIdx(42);
        CHECK(static_cast<usize>(cIdx) == 42);

        auto pfor = graph.parallelFor("PForCoverage", 100, 10, [](usize) noexcept {});
        CHECK(pfor.chunks().size() == 10);

        pfor.domain(TaskDomain::Worker);
        pfor.priority(TaskPriority::High);
    }

    SECTION("TaskGraph queries and metrics") {
        SchedulerConfig config{.workerCount = 1};
        TaskScheduler scheduler(config);
        TaskGraph g;
        CHECK(g.empty());
        CHECK(g.firstWaitNode() == nullptr);
        CHECK(g.pendingTaskCount() == 0);
        CHECK(g.scheduler() == nullptr);
        CHECK(g.arena().slab_count() >= 0);

        g.prepareRun(scheduler);
        CHECK(g.isCompleted());

        g.addPendingTasks(2);
        CHECK(g.pendingTaskCount() == 2);
        g.clear();
        CHECK(g.empty());
    }

    SECTION("TaskGraph span parallelFor overloads and parallelForChunks") {
        SchedulerConfig config{.workerCount = 2};
        TaskScheduler scheduler(config);

        TaskGraph graph;
        std::vector<int> values(10, 1);
        std::span<int> slice(values);

        graph.parallelFor("SpanChunk", slice, 2, [](std::span<int> sub) noexcept {
            for (int& v : sub) {
                v += 10;
            }
        });

        graph.parallelFor(slice, 2, [](int& v, usize idx) noexcept {
            v += static_cast<int>(idx);
        });

        graph.parallelFor(slice, 2, [](int& v) noexcept {
            v += 1;
        });

        std::atomic<u32> chunksExecuted{0};
        graph.parallelForChunks("ChunksTest", 10, 2, [&chunksExecuted](usize) noexcept {
            chunksExecuted.fetch_add(1, std::memory_order_relaxed);
        });

        graph.parallelForChunks(10, 2, [&chunksExecuted](usize) noexcept {
            chunksExecuted.fetch_add(1, std::memory_order_relaxed);
        });

        scheduler.runAndWait(graph);
        CHECK(graph.isCompleted());
        CHECK(chunksExecuted.load(std::memory_order_relaxed) == 10);
    }

    SECTION("Subflow join, detach, and inspect queries") {
        SchedulerConfig config{.workerCount = 2};
        TaskScheduler scheduler(config);

        TaskGraph graph;
        std::atomic<bool> joinedExecuted{false};
        std::atomic<bool> detachedExecuted{false};

        graph.emplaceSubflow([&](Subflow& sf) noexcept {
            CHECK(sf.empty());
            CHECK(sf.size() == 0);
            CHECK(sf.parentNode() != nullptr);
            CHECK(sf.graph() != nullptr);
            CHECK(sf.arena().slab_count() >= 0);

            sf.emplace([&joinedExecuted]() noexcept {
                joinedExecuted.store(true, std::memory_order_release);
            });
            CHECK_FALSE(sf.empty());
            CHECK(sf.size() == 1);

            sf.join();
            // Idempotent join
            sf.join();
        });

        graph.emplaceSubflow([&](Subflow& sf) noexcept {
            sf.emplace([&detachedExecuted]() noexcept {
                detachedExecuted.store(true, std::memory_order_release);
            });
            sf.detach();
        });

        scheduler.runAndWait(graph);
        CHECK(graph.isCompleted());
        CHECK(joinedExecuted.load(std::memory_order_acquire));
        CHECK(detachedExecuted.load(std::memory_order_acquire));
    }
}


