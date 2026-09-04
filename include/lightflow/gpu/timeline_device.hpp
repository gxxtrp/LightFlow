#pragma once

#include <lightflow/gpu/timeline_sync_point.hpp>

namespace lf {

/// Abstract interface for external GPU device or driver querying.
/// Enables idle worker threads to poll timeline progress during work-stealing backoff
/// without blocking the caller or stalling CPU execution.
class ITimelineDevice {
public:
    virtual ~ITimelineDevice() = default;

    /// Queries the current monotonic completed value for the given timeline handle.
    /// Must be non-blocking and safe to call concurrently from multiple worker threads.
    virtual u64 getCompletedValue(TimelineHandle handle) noexcept = 0;
};

} // namespace lf
