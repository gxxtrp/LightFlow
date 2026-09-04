#pragma once

#include <lightflow/core/types.hpp>

#include <chrono>
#include <cstdint>
#include <functional>

namespace lf {

/// 64-bit monotonic timeline handle identifying a GPU timeline semaphore or queue progress monitor.
struct TimelineHandle {
    u64 id{0};

    friend constexpr bool operator==(TimelineHandle lhs, TimelineHandle rhs) noexcept = default;
};

/// Synchronization barrier target on a TimelineHandle with an associated monotonic completion value
/// and a watchdog timeout guarding against GPU TDRs and device loss.
struct TimelineSyncPoint {
    TimelineHandle handle{};
    u64 value{0};
    std::chrono::milliseconds timeoutMs{2000}; // 2-second default for TDR safety
};

} // namespace lf

template <>
struct std::hash<lf::TimelineHandle> {
    std::size_t operator()(lf::TimelineHandle h) const noexcept {
        return std::hash<lf::u64>{}(h.id);
    }
};
