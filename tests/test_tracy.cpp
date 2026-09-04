#include <catch2/catch_test_macros.hpp>
#include <lightflow/lightflow.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

TEST_CASE("Tracy profiler: Macro expansion and zone emission validation", "[tracy][profiler]") {
#if defined(LF_ENABLE_TRACY) && LF_ENABLE_TRACY
    SECTION("Tracy instrumentation active under LF_ENABLE_TRACY=ON") {
        CHECK(tracy::ProfilerAvailable());

        // Basic zone emissions
        {
            {
                LF_ZONE_SCOPED;
            }
            {
                LF_ZONE_SCOPED_N("TestScopedZone");
            }
            {
                LF_ZONE_SCOPED_COLOR(0x00FFCC);
            }
            LF_ZONE_NAMED(namedZone, "TestNamedZone");
            LF_ZONE_NAMED_COLOR(coloredZone, "TestColoredZone", 0xFF00AA);

            LF_SET_THREAD_NAME("TracyTestThread");
            LF_FRAME_MARK;
            LF_FRAME_MARK_NAMED("BenchmarkFrame");
        }

        SUCCEED("Tracy profiling macros successfully executed and emitted");
    }
#else
    SECTION("Tracy macros expand to zero-cost no-ops when disabled") {
        {
            LF_ZONE_SCOPED;
        }
        {
            LF_ZONE_SCOPED_N("NoopZone");
        }
        {
            LF_ZONE_SCOPED_COLOR(0x00FFCC);
        }
        LF_ZONE_NAMED(namedZone, "NoopNamedZone");
        LF_ZONE_NAMED_COLOR(coloredZone, "NoopColoredZone", 0xFF00AA);

        LF_SET_THREAD_NAME("NoopThread");
        LF_FRAME_MARK;
        LF_FRAME_MARK_NAMED("NoopFrame");

        SUCCEED("Tracy macros expand to no-ops when disabled");
    }
#endif
}

TEST_CASE("Tracy profiler: Full TaskGraph and scheduler instrumentation under load", "[tracy][profiler]") {
    lf::SchedulerConfig config{.workerCount = 4, .threadNamePrefix = "LF-TracyWorker"};
    lf::TaskScheduler scheduler(config);

    lf::TaskGraph graph;
    std::atomic<lf::usize> executedCount{0};

    LF_FRAME_MARK_NAMED("StartGraphFrame");

    // Static tasks
    auto t1 = graph.emplace("TracyTask1", [&]() noexcept {
        LF_ZONE_SCOPED_N("Task1Workload");
        executedCount.fetch_add(1, std::memory_order_relaxed);
    });

    auto t2 = graph.emplace("TracyTask2", [&]() noexcept {
        LF_ZONE_SCOPED_N("Task2Workload");
        executedCount.fetch_add(1, std::memory_order_relaxed);
    });

    // Parallel-for
    auto pf = graph.parallelFor("TracyParallelFor", 1000, 100, [&](lf::usize start, lf::usize end) noexcept {
        LF_ZONE_SCOPED_N("ParallelForChunk");
        executedCount.fetch_add(end - start, std::memory_order_relaxed);
    });

    // Dynamic subflow
    auto sf = graph.emplaceSubflow("TracySubflow", [&](lf::Subflow& childGraph) noexcept {
        LF_ZONE_SCOPED_N("SubflowBuild");
        auto s1 = childGraph.emplace([&]() noexcept {
            executedCount.fetch_add(1, std::memory_order_relaxed);
        });
        auto s2 = childGraph.emplace([&]() noexcept {
            executedCount.fetch_add(1, std::memory_order_relaxed);
        });
        s1.precede(s2);
    });

    // Dependency chaining
    t1.precede(t2);
    t2.precede(pf);
    pf.precede(sf);

    lf::Status status = scheduler.runAndWait(graph);
    CHECK(status == lf::Status::Success);
    CHECK(executedCount.load(std::memory_order_acquire) == 2 + 1000 + 2);

    LF_FRAME_MARK_NAMED("EndGraphFrame");
    LF_FRAME_MARK;

    graph.clear();
    CHECK(graph.empty());
}

TEST_CASE("Tracy profiler: Live interactive capture session", "[tracy][interactive][.manual]") {
#if defined(LF_ENABLE_TRACY) && LF_ENABLE_TRACY
    lf::SchedulerConfig config{.workerCount = 8};
    lf::TaskScheduler scheduler(config);

    std::printf("\n===================================================================\n");
    std::printf(" LightFlow Tracy Live Profiler Session\n");
    std::printf(" Connecting to Tracy Profiler GUI on 127.0.0.1:8086...\n");
    std::printf(" Streaming 300 simulated rendering frames (~5 seconds)...\n");
    std::printf("===================================================================\n\n");

    for (int frame = 0; frame < 300; ++frame) {
        LF_FRAME_MARK_NAMED("MainRenderLoop");

        lf::TaskGraph graph;

        auto cullTask = graph.emplace("CullScene", []() noexcept {
            LF_ZONE_SCOPED_N("FrustumCulling");
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        });

        auto pf = graph.parallelFor("ComputeTransforms", 1000, 100, [](lf::usize start, lf::usize end) noexcept {
            LF_ZONE_SCOPED_N("TransformChunk");
            (void)start; (void)end;
        });

        auto postProcess = graph.emplaceSubflow("PostProcessSubflow", [](lf::Subflow& sub) noexcept {
            LF_ZONE_SCOPED_N("BuildBloomPasses");
            auto downsample = sub.emplace([]() noexcept {
                LF_ZONE_SCOPED_N("DownsampleBloom");
                std::this_thread::sleep_for(std::chrono::microseconds(20));
            });
            auto upsample = sub.emplace([]() noexcept {
                LF_ZONE_SCOPED_N("UpsampleBloom");
                std::this_thread::sleep_for(std::chrono::microseconds(20));
            });
            downsample.precede(upsample);
        });

        cullTask.precede(pf);
        pf.precede(postProcess);

        scheduler.runAndWait(graph);
        graph.clear();

        LF_FRAME_MARK;
        std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60 FPS rate
    }

    std::printf("[TRACY] Live capture session finished! Check your Tracy window.\n\n");
    SUCCEED("Live interactive capture completed");
#endif
}
