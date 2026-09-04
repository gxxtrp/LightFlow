#pragma once

#include <lightflow/core/types.hpp>
#include <lightflow/gpu/timeline_sync_point.hpp>

#include <cstdint>
#include <span>

namespace lf {

/// High-level GPU queue family category.
enum class GpuQueue : u8 {
    Graphics = 0, ///< Primary graphics queue (renders, compute, transfer)
    Compute = 1,  ///< Async compute queue (compute shaders, transfer)
    Transfer = 2  ///< Dedicated DMA transfer / copy queue
};

inline constexpr usize GPU_QUEUE_COUNT = 3;

/// Pipeline stage bitmask representing GPU execution barrier targets.
enum class GpuStage : u64 {
    None                    = 0,
    TopOfPipe               = 1ULL << 0,
    DrawIndirect            = 1ULL << 1,
    VertexInput             = 1ULL << 2,
    VertexShader            = 1ULL << 3,
    TessellationControl     = 1ULL << 4,
    TessellationEval        = 1ULL << 5,
    GeometryShader          = 1ULL << 6,
    FragmentShader          = 1ULL << 7,
    EarlyFragmentTests      = 1ULL << 8,
    LateFragmentTests       = 1ULL << 9,
    ColorAttachmentOutput   = 1ULL << 10,
    ComputeShader           = 1ULL << 11,
    Transfer                = 1ULL << 12,
    BottomOfPipe            = 1ULL << 13,
    Host                    = 1ULL << 14,
    AllGraphics             = 1ULL << 15,
    AllCommands             = 1ULL << 16,
    Copy                    = 1ULL << 17,
    Resolve                 = 1ULL << 18,
    Blit                    = 1ULL << 19,
    Clear                   = 1ULL << 20,
    IndexInput              = 1ULL << 21,
    VertexAttributeInput    = 1ULL << 22,
    PreRasterizationShaders = 1ULL << 23
};

constexpr GpuStage operator|(GpuStage lhs, GpuStage rhs) noexcept {
    return static_cast<GpuStage>(static_cast<u64>(lhs) | static_cast<u64>(rhs));
}

constexpr GpuStage operator&(GpuStage lhs, GpuStage rhs) noexcept {
    return static_cast<GpuStage>(static_cast<u64>(lhs) & static_cast<u64>(rhs));
}

constexpr GpuStage operator^(GpuStage lhs, GpuStage rhs) noexcept {
    return static_cast<GpuStage>(static_cast<u64>(lhs) ^ static_cast<u64>(rhs));
}

constexpr GpuStage operator~(GpuStage val) noexcept {
    return static_cast<GpuStage>(~static_cast<u64>(val));
}

constexpr GpuStage& operator|=(GpuStage& lhs, GpuStage rhs) noexcept {
    lhs = lhs | rhs;
    return lhs;
}

constexpr GpuStage& operator&=(GpuStage& lhs, GpuStage rhs) noexcept {
    lhs = lhs & rhs;
    return lhs;
}

constexpr GpuStage& operator^=(GpuStage& lhs, GpuStage rhs) noexcept {
    lhs = lhs ^ rhs;
    return lhs;
}

constexpr bool hasStage(GpuStage mask, GpuStage flag) noexcept {
    return (static_cast<u64>(mask) & static_cast<u64>(flag)) != 0;
}

/// Detailed GPU timeline synchronization descriptor pairing a sync point with a pipeline stage.
struct GpuSyncDesc {
    TimelineSyncPoint syncPoint{};
    GpuStage stage{GpuStage::None}; ///< GpuStage flag; if None, queue default is used
    u64 stageMaskOverride{0};       ///< Optional direct RHI stage mask override (e.g. VkPipelineStageFlags2)
    u32 deviceIndex{0};             ///< Physical device index for device group submissions
};

/// High-level GPU submission batch representation.
/// Passed entirely by non-owning slices (std::span) with zero steady-state heap overhead.
struct GpuSubmission {
    GpuQueue queue{GpuQueue::Graphics};
    u32 submitFlags{0};

    /// Recorded command buffers to execute (e.g. VkCommandBuffer handles cast to void* or direct pointers)
    std::span<const void* const> commandBuffers{};

    /// Wait synchronizations with explicit per-syncpoint stages
    std::span<const GpuSyncDesc> waits{};

    /// Signal synchronizations with explicit per-syncpoint stages
    std::span<const GpuSyncDesc> signals{};

    /// Convenience wait sync points (evaluated using default wait stage)
    std::span<const TimelineSyncPoint> waitSyncPoints{};

    /// Convenience signal sync points (evaluated using default signal stage)
    std::span<const TimelineSyncPoint> signalSyncPoints{};

    /// Optional default stage mask overrides for the convenience spans
    u64 defaultWaitStageMask{0};
    u64 defaultSignalStageMask{0};
};

} // namespace lf
