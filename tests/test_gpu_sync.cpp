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
