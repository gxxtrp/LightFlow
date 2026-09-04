#include <catch2/catch_test_macros.hpp>
#include <lightflow/lightflow.hpp>
#include <lightflow/vulkan/vulkan_sync.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace lf;
using namespace lf::vk;

namespace {

class MockTimelineDevice : public ITimelineDevice {
public:
    void advance(TimelineHandle handle, u64 value) noexcept {
        m_value.store(value, std::memory_order_release);
        m_handle = handle;
    }

    u64 getCompletedValue(TimelineHandle handle) noexcept override {
        if (handle == m_handle) {
            return m_value.load(std::memory_order_acquire);
        }
        return 0;
    }

private:
    TimelineHandle m_handle{};
    std::atomic<u64> m_value{0};
};

} // namespace

// =============================================================================
// TimelineSyncPoint Properties and Reactor Non-Blocking CPU Execution
// =============================================================================

TEST_CASE("TimelineSyncPoint properties and non-blocking CPU execution", "[gpu][timeline]") {
    TimelineHandle h1{100};
    TimelineHandle h2{100};
    TimelineHandle h3{200};

    CHECK(h1 == h2);
    CHECK_FALSE(h1 == h3);

    TimelineSyncPoint sp{h1, 42, std::chrono::milliseconds{500}};
    CHECK(sp.handle == h1);
    CHECK(sp.value == 42);
    CHECK(sp.timeoutMs.count() == 500);

    SchedulerConfig config{.workerCount = 4};
    TaskScheduler scheduler(config);

    TaskGraph graph;
    TimelineHandle handle{1};
    TimelineSyncPoint syncPoint{handle, 10, std::chrono::milliseconds{5000}};

    std::atomic<bool> gpuTaskExecuted{false};
    auto gpuTask = graph.emplace("GpuFeedback", [&]() {
        gpuTaskExecuted.store(true, std::memory_order_release);
    });
    gpuTask.waits(syncPoint);

    // 20 independent CPU tasks that proceed while GPU task waits
    constexpr usize CPU_TASK_COUNT = 20;
    std::atomic<usize> cpuTasksCompleted{0};
    for (usize i = 0; i < CPU_TASK_COUNT; ++i) {
        graph.emplace([&]() {
            cpuTasksCompleted.fetch_add(1, std::memory_order_relaxed);
        });
    }

    graph.prepareRun(scheduler);

    // Wait for all CPU tasks to finish
    auto start = std::chrono::steady_clock::now();
    while (cpuTasksCompleted.load(std::memory_order_acquire) < CPU_TASK_COUNT &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(2)) {
        std::this_thread::yield();
    }
    CHECK(cpuTasksCompleted.load(std::memory_order_acquire) == CPU_TASK_COUNT);
    CHECK_FALSE(gpuTaskExecuted.load(std::memory_order_acquire));

    // Signal timeline value -> unblocks GPU task
    scheduler.notifyTimelineAdvanced(handle, 10);

    start = std::chrono::steady_clock::now();
    while (!gpuTaskExecuted.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(2)) {
        std::this_thread::yield();
    }
    CHECK(gpuTaskExecuted.load(std::memory_order_acquire));

    scheduler.helpUntil([&graph]() noexcept { return graph.isCompleted(); });
    CHECK(graph.isCompleted());
}

// =============================================================================
// Watchdog Timeout Recovery
// =============================================================================

TEST_CASE("Timeout watchdog cancels stalled task and cascades skip downstream", "[gpu][watchdog]") {
    SchedulerConfig config{.workerCount = 4};
    TaskScheduler scheduler(config);

    TaskGraph graph;
    TimelineHandle handle{99};
    // Very short timeout: 30ms
    TimelineSyncPoint syncPoint{handle, 100, std::chrono::milliseconds{30}};

    std::atomic<bool> stalledExecuted{false};
    std::atomic<bool> downstreamExecuted{false};

    auto stalledTask = graph.emplace("StalledTask", [&]() {
        stalledExecuted.store(true, std::memory_order_release);
    });
    stalledTask.waits(syncPoint);

    auto downstreamTask = graph.emplace("DownstreamTask", [&]() {
        downstreamExecuted.store(true, std::memory_order_release);
    });
    stalledTask >> downstreamTask;

    scheduler.runAndWait(graph);

    // Both stalled and dependent downstream task must be skipped, not executed
    CHECK_FALSE(stalledExecuted.load(std::memory_order_acquire));
    CHECK_FALSE(downstreamExecuted.load(std::memory_order_acquire));
    CHECK(graph.isCompleted());
}

// =============================================================================
// Vulkan 1.3 Synchronization2 Translation Helpers
// =============================================================================

TEST_CASE("Vulkan 1.3 Synchronization2 mapping and translation", "[gpu][vulkan]") {
    QueueFamilyIndices indices{
        .graphics = 0,
        .compute = 1,
        .transfer = 2
    };

    CHECK(toQueueFamilyIndex(GpuQueue::Graphics, indices) == 0);
    CHECK(toQueueFamilyIndex(GpuQueue::Compute, indices) == 1);
    CHECK(toQueueFamilyIndex(GpuQueue::Transfer, indices) == 2);

    CHECK(toVkPipelineStageFlags2(GpuStage::ComputeShader) == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    CHECK(toVkPipelineStageFlags2(GpuStage::ColorAttachmentOutput) == VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);

    // FixedSubmitStorage stack translation
    FixedSubmitStorage<4, 4, 4> storage;
    VkSemaphore rawSem = reinterpret_cast<VkSemaphore>(0x12345);

    TimelineSyncPoint waitPoints[] = {
        {TimelineHandle{0x12345}, 10, std::chrono::milliseconds{1000}}
    };
    TimelineSyncPoint signalPoints[] = {
        {TimelineHandle{0x12345}, 20, std::chrono::milliseconds{1000}}
    };

    GpuSubmission submission{
        .queue = GpuQueue::Graphics,
        .waitSyncPoints = waitPoints,
        .signalSyncPoints = signalPoints
    };

    VkSubmitInfo2 submitInfo = toVkSubmitInfo2(submission, storage.asStorage());

    CHECK(submitInfo.sType == VK_STRUCTURE_TYPE_SUBMIT_INFO_2);
    CHECK(submitInfo.waitSemaphoreInfoCount == 1);
    CHECK(submitInfo.pWaitSemaphoreInfos[0].semaphore == rawSem);
    CHECK(submitInfo.pWaitSemaphoreInfos[0].value == 10);
    CHECK(submitInfo.signalSemaphoreInfoCount == 1);
    CHECK(submitInfo.pSignalSemaphoreInfos[0].semaphore == rawSem);
    CHECK(submitInfo.pSignalSemaphoreInfos[0].value == 20);
}

// =============================================================================
// TimelineReactor Watchdog Concurrency Races
// =============================================================================

TEST_CASE("TimelineReactor concurrent push notification and watchdog timeout race", "[gpu][watchdog][race]") {
    // Stress test the exact atomic race between fast-path notifyTimelineAdvanced()
    // and watchdog checkTimeouts() across multiple rounds with multiple sync points.
    constexpr usize ROUNDS = 50;
    constexpr usize TASKS_PER_ROUND = 20;

    for (usize round = 0; round < ROUNDS; ++round) {
        TimelineReactor reactor;

        struct TestTaskContext {
            TaskNode node{};
            TimelineWaitNode waitNode{};
        };

        std::vector<TestTaskContext> contexts(TASKS_PER_ROUND);
        TimelineHandle handle{static_cast<u64>(5000 + round)};

        for (usize i = 0; i < TASKS_PER_ROUND; ++i) {
            auto& ctx = contexts[i];
            ctx.node.id = static_cast<u32>(i);
            ctx.node.state.store(TaskState::Pending, std::memory_order_relaxed);
            ctx.node.inDegree.store(1, std::memory_order_relaxed);
            ctx.node.initialInDegree = 1;

            ctx.waitNode.task = &ctx.node;
            // Backdate registeredTime so it is expiring right at or before checkTimeouts runs
            ctx.waitNode.syncPoint = TimelineSyncPoint{handle, 10, std::chrono::milliseconds{1}};
            ctx.waitNode.registeredTime = std::chrono::steady_clock::now() - std::chrono::milliseconds{2};
            ctx.waitNode.satisfied.store(false, std::memory_order_relaxed);

            reactor.registerWait(&ctx.waitNode);
        }

        REQUIRE(reactor.pendingCount() == TASKS_PER_ROUND);

        std::atomic<bool> startSignal{false};
        std::atomic<usize> notifyUnblocked{0};
        std::atomic<bool> checkTimeoutResult{false};

        std::thread pushThread([&]() {
            while (!startSignal.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            notifyUnblocked.store(reactor.notifyTimelineAdvanced(handle, 10), std::memory_order_release);
        });

        std::thread watchdogThread([&]() {
            while (!startSignal.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            checkTimeoutResult.store(reactor.checkTimeouts(), std::memory_order_release);
        });

        startSignal.store(true, std::memory_order_release);

        pushThread.join();
        watchdogThread.join();

        // Exactly-once invariant: every task must have satisfied == true,
        // and its state is either Timeout (if watchdog won) or Pending with inDegree decremented (if notify won).
        usize completedByPush = 0;
        usize completedByTimeout = 0;

        for (usize i = 0; i < TASKS_PER_ROUND; ++i) {
            auto& ctx = contexts[i];
            CHECK(ctx.waitNode.satisfied.load(std::memory_order_relaxed));

            TaskState st = ctx.node.state.load(std::memory_order_relaxed);
            if (st == TaskState::Timeout) {
                completedByTimeout++;
            } else {
                completedByPush++;
            }
        }

        CHECK(completedByPush + completedByTimeout == TASKS_PER_ROUND);
        CHECK(notifyUnblocked.load(std::memory_order_relaxed) == completedByPush);
        CHECK(reactor.pendingCount() == 0);
        CHECK_FALSE(reactor.hasPending());
    }
}

TEST_CASE("TaskScheduler concurrent notifyTimelineAdvanced and watchdog timeout race on TaskGraph", "[gpu][watchdog][race][integration]") {
    constexpr usize ROUNDS = 20;

    for (usize round = 0; round < ROUNDS; ++round) {
        SchedulerConfig config{.workerCount = 4};
        TaskScheduler scheduler(config);

        TaskGraph graph;
        TimelineHandle handle{static_cast<u64>(8000 + round)};
        // Very tight 1ms timeout
        TimelineSyncPoint syncPoint{handle, 100, std::chrono::milliseconds{1}};

        std::atomic<u32> executedTasks{0};
        std::atomic<u32> timeoutTasks{0};
        constexpr usize TASK_COUNT = 10;

        std::vector<TaskHandle> tasks;
        tasks.reserve(TASK_COUNT);

        for (usize i = 0; i < TASK_COUNT; ++i) {
            auto t = graph.emplace([&executedTasks]() noexcept {
                executedTasks.fetch_add(1, std::memory_order_relaxed);
            });
            t.waits(syncPoint);
            tasks.push_back(t);
        }

        std::atomic<bool> startSignal{false};

        std::thread signalerThread([&]() {
            while (!startSignal.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            scheduler.notifyTimelineAdvanced(handle, 100);
        });

        startSignal.store(true, std::memory_order_release);
        scheduler.runAndWait(graph);
        signalerThread.join();

        for (auto& t : tasks) {
            TaskState st = t.node()->state.load(std::memory_order_relaxed);
            if (st == TaskState::Timeout) {
                timeoutTasks.fetch_add(1, std::memory_order_relaxed);
            }
        }

        CHECK(executedTasks.load(std::memory_order_relaxed) + timeoutTasks.load(std::memory_order_relaxed) == TASK_COUNT);
        CHECK(graph.isCompleted());
    }
}

TEST_CASE("GpuStage bitwise operators, TimelineReactor pollDevice, and Vulkan sync helpers", "[gpu][coverage]") {
    SECTION("GpuStage bitwise operators") {
        GpuStage s1 = GpuStage::VertexShader;
        GpuStage s2 = GpuStage::FragmentShader;
        GpuStage combined = s1 | s2;
        CHECK((combined & s1) == s1);
        CHECK((combined & s2) == s2);
        CHECK((s1 ^ s2) == combined);
        CHECK(~s1 != s1);

        GpuStage acc = s1;
        acc |= s2;
        CHECK(acc == combined);
        acc &= s1;
        CHECK(acc == s1);
        acc ^= s1;
        CHECK(acc == GpuStage::None);
    }

    SECTION("TimelineHandle hash") {
        TimelineHandle h1{42};
        TimelineHandle h2{42};
        TimelineHandle h3{99};
        CHECK(std::hash<TimelineHandle>{}(h1) == std::hash<TimelineHandle>{}(h2));
        CHECK(std::hash<TimelineHandle>{}(h1) != std::hash<TimelineHandle>{}(h3));
    }

    SECTION("TimelineReactor pollDevice") {
        TimelineReactor reactor;
        MockTimelineDevice device;
        TimelineHandle handle{123};

        // When empty
        CHECK(reactor.pollDevice(device) == 0);

        TaskNode node{};
        node.inDegree.store(1, std::memory_order_relaxed);
        TimelineWaitNode waitNode{};
        waitNode.task = &node;
        waitNode.syncPoint = TimelineSyncPoint{handle, 10, std::chrono::milliseconds{1000}};
        waitNode.satisfied.store(false, std::memory_order_relaxed);

        reactor.registerWait(&waitNode);
        CHECK(reactor.hasPending());

        // Device has not reached 10
        device.advance(handle, 5);
        CHECK(reactor.pollDevice(device) == 0);
        CHECK_FALSE(waitNode.satisfied.load(std::memory_order_relaxed));

        // Device reaches 10
        device.advance(handle, 10);
        CHECK(reactor.pollDevice(device) == 1);
        CHECK(waitNode.satisfied.load(std::memory_order_relaxed));
        CHECK((node.inDegree.load(std::memory_order_relaxed) & 0xFFFFu) == 0);
    }

    SECTION("Vulkan queue family and flags translation") {
        QueueFamilyIndices indices{0, 1, 2};
        CHECK(toQueueFamilyIndex(GpuQueue::Graphics, indices) == 0);
        CHECK(toQueueFamilyIndex(GpuQueue::Compute, indices) == 1);
        CHECK(toQueueFamilyIndex(GpuQueue::Transfer, indices) == 2);

        CHECK(toQueueFamilyIndex(GpuQueue::Graphics, 10, 20, 30) == 10);
        CHECK(toQueueFamilyIndex(GpuQueue::Compute, 10, 20, 30) == 20);
        CHECK(toQueueFamilyIndex(GpuQueue::Transfer, 10, 20, 30) == 30);

        CHECK(toVkQueueFlags(GpuQueue::Graphics) != 0);
        CHECK(toVkQueueFlags(GpuQueue::Compute) != 0);
        CHECK(toVkQueueFlags(GpuQueue::Transfer) != 0);

        VkSemaphore sem = reinterpret_cast<VkSemaphore>(static_cast<uintptr_t>(0x1234));
        CHECK(toTimelineHandle(sem).id == static_cast<u64>(reinterpret_cast<uintptr_t>(sem)));

        int dummy = 42;
        VkCommandBuffer cmd = toVkCommandBuffer(&dummy);
        CHECK(cmd == reinterpret_cast<VkCommandBuffer>(&dummy));

        VkCommandBufferSubmitInfo cmdInfo = toVkCommandBufferSubmitInfo(cmd, 1);
        CHECK(cmdInfo.sType == VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO);
        CHECK(cmdInfo.commandBuffer == cmd);
        CHECK(cmdInfo.deviceMask == 1);

        VkCommandBufferSubmitInfo cmdInfoVoid = toVkCommandBufferSubmitInfo(static_cast<const void*>(&dummy), 2);
        CHECK(cmdInfoVoid.commandBuffer == cmd);
        CHECK(cmdInfoVoid.deviceMask == 2);
    }

    SECTION("toVkSubmitInfo2 with arena allocation and semaphore descriptor overrides") {
        alignas(SLAB_SIZE) std::array<std::byte, SLAB_SIZE * 2> buffer{};
        BlockPool pool = BlockPool::from_buffer(buffer.data(), buffer.size());
        SlabArena arena(pool);

        TimelineHandle handle{999};
        int cmdDummy = 1;
        const void* cmdPtr = &cmdDummy;

        GpuSyncDesc waitDesc{
            .syncPoint = TimelineSyncPoint{handle, 5},
            .stage = GpuStage::VertexShader,
            .stageMaskOverride = 0,
            .deviceIndex = 0
        };

        GpuSyncDesc sigDesc{
            .syncPoint = TimelineSyncPoint{handle, 10},
            .stage = GpuStage::FragmentShader,
            .stageMaskOverride = 0,
            .deviceIndex = 0
        };

        GpuSubmission subDesc{};
        subDesc.queue = GpuQueue::Graphics;
        subDesc.commandBuffers = std::span<const void* const>(&cmdPtr, 1);
        subDesc.waits = std::span<const GpuSyncDesc>(&waitDesc, 1);
        subDesc.signals = std::span<const GpuSyncDesc>(&sigDesc, 1);

        VkSubmitInfo2 submitInfo = toVkSubmitInfo2(subDesc, arena);
        CHECK(submitInfo.sType == VK_STRUCTURE_TYPE_SUBMIT_INFO_2);
        CHECK(submitInfo.waitSemaphoreInfoCount == 1);
        CHECK(submitInfo.commandBufferInfoCount == 1);
        CHECK(submitInfo.signalSemaphoreInfoCount == 1);
    }
}

