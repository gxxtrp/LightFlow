#include <lightflow/gpu/timeline_reactor.hpp>
#include <lightflow/task/task_node.hpp>
#include <lightflow/task/task_graph.hpp>
#include <lightflow/core/tracy.hpp>

namespace lf {

void TimelineReactor::registerWait(TimelineWaitNode* node) noexcept {
    LF_ASSERT(node != nullptr);
    TimelineWaitNode* oldHead = m_head.load(std::memory_order_relaxed);
    do {
        node->nextInReactor = oldHead;
    } while (!m_head.compare_exchange_weak(
        oldHead, node, std::memory_order_release, std::memory_order_relaxed));
    m_pendingCount.fetch_add(1, std::memory_order_relaxed);
}

usize TimelineReactor::notifyTimelineAdvanced(TimelineHandle handle, u64 completedValue) noexcept {
    LF_ZONE_SCOPED_N("TimelineReactor::notifyTimelineAdvanced");
    if (!hasPending()) {
        return 0;
    }

    usize unblocked = 0;
    TimelineWaitNode* curr = m_head.load(std::memory_order_acquire);
    while (curr != nullptr) {
        if (curr->syncPoint.handle == handle && curr->syncPoint.value <= completedValue) {
            bool expected = false;
            if (curr->satisfied.compare_exchange_strong(expected, true,
                    std::memory_order_acq_rel, std::memory_order_relaxed)) {
                m_pendingCount.fetch_sub(1, std::memory_order_relaxed);
                if (curr->task != nullptr) {
                    curr->task->decrementActive(curr->task->graph);
                    unblocked++;
                }
            }
        }
        curr = curr->nextInReactor;
    }
    return unblocked;
}

usize TimelineReactor::pollDevice(ITimelineDevice& device) noexcept {
    LF_ZONE_SCOPED_N("TimelineReactor::pollDevice");
    if (!hasPending()) {
        return 0;
    }

    usize unblocked = 0;
    TimelineWaitNode* curr = m_head.load(std::memory_order_acquire);
    while (curr != nullptr) {
        if (!curr->satisfied.load(std::memory_order_relaxed)) {
            u64 completed = device.getCompletedValue(curr->syncPoint.handle);
            if (completed >= curr->syncPoint.value) {
                bool expected = false;
                if (curr->satisfied.compare_exchange_strong(expected, true,
                        std::memory_order_acq_rel, std::memory_order_relaxed)) {
                    m_pendingCount.fetch_sub(1, std::memory_order_relaxed);
                    if (curr->task != nullptr) {
                        curr->task->decrementActive(curr->task->graph);
                        unblocked++;
                    }
                }
            }
        }
        curr = curr->nextInReactor;
    }
    return unblocked;
}

bool TimelineReactor::checkTimeouts() noexcept {
    LF_ZONE_SCOPED_N("TimelineReactor::checkTimeouts");
    if (!hasPending()) {
        return false;
    }

    auto now = std::chrono::steady_clock::now();
    bool anyTimeout = false;
    TimelineWaitNode* curr = m_head.load(std::memory_order_acquire);
    while (curr != nullptr) {
        if (!curr->satisfied.load(std::memory_order_relaxed)) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - curr->registeredTime);
            if (elapsed >= curr->syncPoint.timeoutMs) {
                bool expected = false;
                if (curr->satisfied.compare_exchange_strong(expected, true,
                        std::memory_order_acq_rel, std::memory_order_relaxed)) {
                    m_pendingCount.fetch_sub(1, std::memory_order_relaxed);
                    m_hasTimeout.store(true, std::memory_order_release);
                    anyTimeout = true;
                    if (curr->task != nullptr) {
                        curr->task->handleGpuTimeout(curr->task->graph);
                    }
                }
            }
        }
        curr = curr->nextInReactor;
    }
    return anyTimeout;
}

void TimelineReactor::reset() noexcept {
    m_head.store(nullptr, std::memory_order_relaxed);
    m_pendingCount.store(0, std::memory_order_relaxed);
    m_hasTimeout.store(false, std::memory_order_relaxed);
}

} // namespace lf
