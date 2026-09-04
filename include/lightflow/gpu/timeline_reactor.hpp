#pragma once

#include <lightflow/core/types.hpp>
#include <lightflow/core/tracy.hpp>
#include <lightflow/gpu/timeline_sync_point.hpp>
#include <lightflow/gpu/timeline_device.hpp>

#include <atomic>
#include <chrono>

namespace lf {

struct TaskNode;
class TaskGraph;

/// Intrusive wait node representing a task waiting on a specific GPU timeline synchronization barrier.
/// Allocated from the task graph's SlabArena with zero steady-state heap overhead.
struct alignas(lf::CACHELINE_SIZE) TimelineWaitNode {
    TaskNode* task{nullptr};
    TimelineSyncPoint syncPoint{};
    std::chrono::steady_clock::time_point registeredTime{};
    std::atomic<bool> satisfied{false};
    TimelineWaitNode* nextInGraph{nullptr};
    TimelineWaitNode* nextInReactor{nullptr};
};

/// 64-bit monotonic GPU timeline synchronization engine.
/// Manages an inactive wait-set of deferred tasks with 0 ns direct push notifications,
/// idle-worker steal-backoff query fallback, and watchdog timeout handling for GPU TDRs.
class alignas(lf::CACHELINE_SIZE) TimelineReactor {
public:
    TimelineReactor() noexcept = default;
    ~TimelineReactor() noexcept = default;

    TimelineReactor(const TimelineReactor&) = delete;
    TimelineReactor& operator=(const TimelineReactor&) = delete;
    TimelineReactor(TimelineReactor&&) = delete;
    TimelineReactor& operator=(TimelineReactor&&) = delete;

    /// Registers an inactive wait node into the reactor's lock-free wait-set.
    void registerWait(TimelineWaitNode* node) noexcept;

    /// Fast-path push notification unblocking all eligible tasks where syncPoint.value <= completedValue.
    /// Atomically decrements task dependencies and unblocks tasks with 0 ns latency.
    /// Returns the number of newly unblocked tasks.
    usize notifyTimelineAdvanced(TimelineHandle handle, u64 completedValue) noexcept;

    /// Fallback query invoked by idle workers during work-stealing backoff.
    /// Queries the external device for completed values and unblocks any satisfied tasks.
    /// Returns the number of newly unblocked tasks.
    usize pollDevice(ITimelineDevice& device) noexcept;

    /// Scans pending wait nodes for watchdog timeouts (elapsed time > timeoutMs).
    /// Safely cancels stalled tasks, sets their state to Timeout, cascades skips downstream,
    /// and marks the reactor timeout status.
    /// Returns true if any task timed out.
    bool checkTimeouts() noexcept;

    /// Returns true if there are currently pending wait nodes in the wait-set.
    LF_NODISCARD bool hasPending() const noexcept {
        return m_pendingCount.load(std::memory_order_relaxed) > 0;
    }

    /// Returns the number of pending wait nodes in the wait-set.
    LF_NODISCARD u32 pendingCount() const noexcept {
        return m_pendingCount.load(std::memory_order_relaxed);
    }

    /// Returns true if any registered task has encountered a GPU timeout.
    LF_NODISCARD bool hasTimeout() const noexcept {
        return m_hasTimeout.load(std::memory_order_acquire);
    }

    /// Resets the timeout flag.
    void resetTimeout() noexcept {
        m_hasTimeout.store(false, std::memory_order_relaxed);
    }

    /// Resets the reactor state, clearing the wait-set.
    void reset() noexcept;

private:
    LF_ALIGN_CACHELINE std::atomic<TimelineWaitNode*> m_head{nullptr};
    LF_ALIGN_CACHELINE std::atomic<u32> m_pendingCount{0};
    LF_ALIGN_CACHELINE std::atomic<bool> m_hasTimeout{false};
};

} // namespace lf
