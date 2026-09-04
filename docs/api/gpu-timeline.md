# GPU Timeline Semaphore Synchronization API Reference

Modern real-time graphics pipelines require tight coordination between CPU task dispatch and asynchronous GPU execution. Blocking a CPU worker thread with synchronous graphics API calls (such as `vkWaitSemaphores`, `ID3D12Fence::SetEventOnCompletion`, or `clFinish`) wastes valuable CPU cores.

LightFlow provides **first-class, non-blocking GPU timeline synchronization** through [`TimelineSyncPoint`](../../include/lightflow/gpu/timeline_sync_point.hpp) and the [`TimelineReactor`](../../include/lightflow/gpu/timeline_reactor.hpp).

---

## Architectural Mechanics

```mermaid
sequenceDiagram
    participant Main as Main Thread
    participant Scheduler as LightFlow Workers
    participant Reactor as TimelineReactor
    participant GPU as GPU Queue (Vulkan / DX12)

    Main->>GPU: Submit Compute Pass (Signals Timeline Semaphore Value 42)
    Main->>Scheduler: Submit TaskGraph with TimelineSyncPoint(42)
    Note over Scheduler: Task A finishes; Task B has TimelineWaitPoint(42)
    Scheduler->>Reactor: Register Task B in Non-Blocking Wait-Set
    Note over Scheduler: Workers steal and execute independent CPU tasks!
    GPU-->>Reactor: Timeline Semaphore Reaches 42
    Reactor->>Scheduler: Decrement Task B In-Degree -> Unblock Task B
    Scheduler->>Scheduler: Worker Pops Task B & Executes Post-Processing
```

### 1. Non-Blocking Wait-Set (`TimelineReactor`)
When a task node possesses a [`TimelineSyncPoint`](#struct-reference-timelinesyncpoint), its initial in-degree is incremented by 1.
* While upstream CPU dependencies finish, the task remains deferred inside the scheduler's non-blocking `TimelineReactor`.
* **Zero Worker Stalling**: Worker threads never block on the GPU. They immediately continue stealing and executing other CPU-intensive tasks (e.g., audio, physics, animation, or other render passes).
* When the GPU signals the required monotonic value, the reactor decrements the task's in-degree, immediately pushing it into the worker work-stealing pool.

### 2. Hardware Watchdog Timeouts (Anti-TDR Protection)
GPU drivers can hang or trigger Timeout Detection and Recovery (TDR). Every `TimelineSyncPoint` accepts a `timeoutMs` watchdog timer. If the GPU fails to signal the target value within the timeout, the reactor marks the task with `Status::Timeout` and cascades the cancellation downstream, **preventing the entire engine from deadlocking**.

---

## Struct Reference: `TimelineSyncPoint`

```cpp
namespace lf {

struct TimelineSyncPoint {
    /// Opaque handle to the GPU timeline synchronization primitive
    /// (e.g. VkSemaphore, ID3D12Fence*, or id<MTLSharedEvent>).
    u64 device{0};

    /// Monotonic 64-bit counter value required to unblock the task.
    u64 targetValue{0};

    /// Hardware watchdog timeout in milliseconds (0 = infinite).
    /// Default: 1000 ms (1 second).
    u32 timeoutMs{1000};
};

} // namespace lf
```

---

## Attaching Wait Points to Tasks

```cpp
// In TaskHandle:
TaskHandle addWaitPoint(const TimelineSyncPoint& syncPoint) const noexcept;
```

### Usage Example

```cpp
// 1. Submit GPU work that will signal value 105 upon completion
vkQueueSubmit2(gfxQueue, 1, &submitInfo, VK_NULL_HANDLE);

// 2. Build CPU Task Graph
lf::TaskGraph graph;

auto recordShadows = graph.emplace("RecordShadowPass", []() noexcept { /* ... */ });

auto postProcess = graph.emplace("PostProcessWithComputeData", []() noexcept {
    // Guaranteed: GPU compute pass has reached value 105!
});

// 3. Attach GPU timeline condition to postProcess task
postProcess.addWaitPoint(lf::TimelineSyncPoint{
    .device = reinterpret_cast<lf::u64>(vkComputeTimelineSemaphore),
    .targetValue = 105,
    .timeoutMs = 50 // Watchdog guards against GPU TDR
});

recordShadows.precede(postProcess);
```

---

## Interface Reference: `ITimelineDevice`

To enable automatic background polling or querying of GPU progress, implement [`ITimelineDevice`](../../include/lightflow/gpu/timeline_device.hpp) and pass it to `SchedulerConfig::timelineDevice`.

```cpp
namespace lf {

class ITimelineDevice {
public:
    virtual ~ITimelineDevice() = default;

    /// Queries the current monotonic value of the timeline primitive.
    virtual u64 queryCurrentValue(u64 deviceHandle) noexcept = 0;

    /// Optional progress kick called periodically when workers are idle.
    virtual void poll() noexcept = 0;
};

} // namespace lf
```

---

## Graphics API Integration Recipes

### 1. Vulkan 1.3 Synchronization2 (`VK_KHR_synchronization2`)

Vulkan 1.3 core timeline semaphores map directly to LightFlow:

```cpp
#include <lightflow/lightflow.hpp>
#include <vulkan/vulkan.h>

class VulkanTimelineDevice final : public lf::ITimelineDevice {
public:
    explicit VulkanTimelineDevice(VkDevice device) : m_device(device) {}

    lf::u64 queryCurrentValue(lf::u64 deviceHandle) noexcept override {
        VkSemaphore semaphore = reinterpret_cast<VkSemaphore>(deviceHandle);
        uint64_t currentValue = 0;
        VkResult res = vkGetSemaphoreCounterValue(m_device, semaphore, &currentValue);
        return (res == VK_SUCCESS) ? currentValue : 0;
    }

    void poll() noexcept override {
        // Optional: Trigger query pool recycles or staging buffer cleanups
    }

private:
    VkDevice m_device;
};
```

---

### 2. DirectX 12 (`ID3D12Fence`)

DirectX 12 monotonic fences seamlessly bind to `ITimelineDevice`:

```cpp
#include <lightflow/lightflow.hpp>
#include <d3d12.h>

class D3D12TimelineDevice final : public lf::ITimelineDevice {
public:
    lf::u64 queryCurrentValue(lf::u64 deviceHandle) noexcept override {
        auto* fence = reinterpret_cast<ID3D12Fence*>(deviceHandle);
        return fence->GetCompletedValue();
    }

    void poll() noexcept override {}
};
```

---

### 3. Apple Metal (`MTLSharedEvent`)

Metal's shared events provide monotonic 64-bit timeline progression on macOS and iOS:

```objc
#include <lightflow/lightflow.hpp>
#import <Metal/Metal.h>

class MetalTimelineDevice final : public lf::ITimelineDevice {
public:
    lf::u64 queryCurrentValue(lf::u64 deviceHandle) noexcept override {
        id<MTLSharedEvent> event = (__bridge id<MTLSharedEvent>)(void*)deviceHandle;
        return event.signaledValue;
    }

    void poll() noexcept override {}
};
```

---

## Direct Engine Push Notification (`notifyTimelineAdvanced`)

For the lowest possible latency (**0 ns CPU resumption**), call `notifyTimelineAdvanced` immediately following a GPU completion interrupt or command buffer retire handler:

```cpp
// Called directly from RHI completion thread or frame fence callback:
scheduler.timelineReactor().notifyTimelineAdvanced(
    reinterpret_cast<lf::u64>(vkTimelineSemaphore),
    completedValue
);
```
This immediately wakes all matching deferred CPU tasks and injects them directly into the worker work-stealing pool without waiting for a polling interval.
