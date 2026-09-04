#pragma once

#if __has_include(<vulkan/vulkan.h>)
    #include <vulkan/vulkan.h>
    #define LF_HAS_NATIVE_VULKAN 1
#elif defined(LF_ENABLE_VULKAN_HELPERS) || defined(LF_MOCK_VULKAN)
    #include <lightflow/vulkan/vulkan_mock.hpp>
    #define LF_HAS_NATIVE_VULKAN 0
#else
    #error "Vulkan headers (<vulkan/vulkan.h>) not found. Define LF_ENABLE_VULKAN_HELPERS to enable fallback definitions."
#endif

#include <lightflow/core/types.hpp>
#include <lightflow/core/slab_arena.hpp>
#include <lightflow/gpu/timeline_sync_point.hpp>
#include <lightflow/gpu/gpu_submission.hpp>

#include <array>
#include <cstdint>
#include <span>

namespace lf::vk {

/// Configuration for mapping LightFlow GPU queue families to physical Vulkan queue family indices.
struct QueueFamilyIndices {
    u32 graphics{0};
    u32 compute{0};
    u32 transfer{0};
};

/// Resolves the Vulkan queue family index corresponding to a GpuQueue enum.
constexpr u32 toQueueFamilyIndex(GpuQueue queue, const QueueFamilyIndices& indices) noexcept {
    switch (queue) {
        case GpuQueue::Graphics: return indices.graphics;
        case GpuQueue::Compute:  return indices.compute;
        case GpuQueue::Transfer: return indices.transfer;
    }
    return indices.graphics;
}

/// Resolves the Vulkan queue family index using explicit per-queue indices.
constexpr u32 toQueueFamilyIndex(GpuQueue queue, u32 graphicsIndex, u32 computeIndex, u32 transferIndex) noexcept {
    switch (queue) {
        case GpuQueue::Graphics: return graphicsIndex;
        case GpuQueue::Compute:  return computeIndex;
        case GpuQueue::Transfer: return transferIndex;
    }
    return graphicsIndex;
}

/// Maps GpuQueue to standard Vulkan queue capability bitflags.
constexpr VkQueueFlags toVkQueueFlags(GpuQueue queue) noexcept {
    switch (queue) {
        case GpuQueue::Graphics:
            return VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
        case GpuQueue::Compute:
            return VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
        case GpuQueue::Transfer:
            return VK_QUEUE_TRANSFER_BIT;
    }
    return 0;
}

/// Converts GpuStage bitmask flags to Vulkan 1.3 VkPipelineStageFlags2.
constexpr VkPipelineStageFlags2 toVkPipelineStageFlags2(GpuStage stage) noexcept {
    VkPipelineStageFlags2 flags = 0;
    if (hasStage(stage, GpuStage::TopOfPipe)) flags |= VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    if (hasStage(stage, GpuStage::DrawIndirect)) flags |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    if (hasStage(stage, GpuStage::VertexInput)) flags |= VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
    if (hasStage(stage, GpuStage::VertexShader)) flags |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    if (hasStage(stage, GpuStage::TessellationControl)) flags |= VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT;
    if (hasStage(stage, GpuStage::TessellationEval)) flags |= VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT;
    if (hasStage(stage, GpuStage::GeometryShader)) flags |= VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT;
    if (hasStage(stage, GpuStage::FragmentShader)) flags |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    if (hasStage(stage, GpuStage::EarlyFragmentTests)) flags |= VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
    if (hasStage(stage, GpuStage::LateFragmentTests)) flags |= VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    if (hasStage(stage, GpuStage::ColorAttachmentOutput)) flags |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (hasStage(stage, GpuStage::ComputeShader)) flags |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    if (hasStage(stage, GpuStage::Transfer)) flags |= VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    if (hasStage(stage, GpuStage::BottomOfPipe)) flags |= VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
    if (hasStage(stage, GpuStage::Host)) flags |= VK_PIPELINE_STAGE_2_HOST_BIT;
    if (hasStage(stage, GpuStage::AllGraphics)) flags |= VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
    if (hasStage(stage, GpuStage::AllCommands)) flags |= VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    if (hasStage(stage, GpuStage::Copy)) flags |= VK_PIPELINE_STAGE_2_COPY_BIT;
    if (hasStage(stage, GpuStage::Resolve)) flags |= VK_PIPELINE_STAGE_2_RESOLVE_BIT;
    if (hasStage(stage, GpuStage::Blit)) flags |= VK_PIPELINE_STAGE_2_BLIT_BIT;
    if (hasStage(stage, GpuStage::Clear)) flags |= VK_PIPELINE_STAGE_2_CLEAR_BIT;
    if (hasStage(stage, GpuStage::IndexInput)) flags |= VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
    if (hasStage(stage, GpuStage::VertexAttributeInput)) flags |= VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT;
    if (hasStage(stage, GpuStage::PreRasterizationShaders)) flags |= VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT;
    return flags;
}

/// Returns the sensible default pipeline stage mask for wait operations on the given queue.
constexpr VkPipelineStageFlags2 getDefaultWaitStage(GpuQueue queue) noexcept {
    switch (queue) {
        case GpuQueue::Graphics: return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        case GpuQueue::Compute:  return VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        case GpuQueue::Transfer: return VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    }
    return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
}

/// Returns the sensible default pipeline stage mask for signal operations on the given queue.
constexpr VkPipelineStageFlags2 getDefaultSignalStage(GpuQueue queue) noexcept {
    switch (queue) {
        case GpuQueue::Graphics: return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        case GpuQueue::Compute:  return VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        case GpuQueue::Transfer: return VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    }
    return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
}

/// Converts a 64-bit TimelineHandle to a native VkSemaphore handle.
inline VkSemaphore toVkSemaphore(TimelineHandle handle) noexcept {
    return reinterpret_cast<VkSemaphore>(static_cast<uintptr_t>(handle.id));
}

/// Converts a native VkSemaphore handle to a 64-bit TimelineHandle.
inline TimelineHandle toTimelineHandle(VkSemaphore semaphore) noexcept {
    return TimelineHandle{static_cast<u64>(reinterpret_cast<uintptr_t>(semaphore))};
}

/// Converts an opaque pointer to a VkCommandBuffer handle.
inline VkCommandBuffer toVkCommandBuffer(const void* handle) noexcept {
    return reinterpret_cast<VkCommandBuffer>(const_cast<void*>(handle));
}

/// Maps a TimelineSyncPoint and stage mask directly to a Vulkan 1.3 VkSemaphoreSubmitInfo.
inline VkSemaphoreSubmitInfo toVkSemaphoreSubmitInfo(
    const TimelineSyncPoint& syncPoint,
    VkPipelineStageFlags2 stageMask,
    u32 deviceIndex = 0
) noexcept {
    VkSemaphoreSubmitInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    info.pNext = nullptr;
    info.semaphore = toVkSemaphore(syncPoint.handle);
    info.value = syncPoint.value;
    info.stageMask = stageMask;
    info.deviceIndex = deviceIndex;
    return info;
}

/// Maps a GpuSyncDesc descriptor to a Vulkan 1.3 VkSemaphoreSubmitInfo using the provided default stage fallback.
inline VkSemaphoreSubmitInfo toVkSemaphoreSubmitInfo(
    const GpuSyncDesc& desc,
    VkPipelineStageFlags2 defaultStage
) noexcept {
    VkPipelineStageFlags2 stage = desc.stageMaskOverride;
    if (stage == 0) {
        stage = toVkPipelineStageFlags2(desc.stage);
        if (stage == 0) {
            stage = defaultStage;
        }
    }
    return toVkSemaphoreSubmitInfo(desc.syncPoint, stage, desc.deviceIndex);
}

/// Maps a VkCommandBuffer to a Vulkan 1.3 VkCommandBufferSubmitInfo.
inline VkCommandBufferSubmitInfo toVkCommandBufferSubmitInfo(
    VkCommandBuffer cmd,
    u32 deviceMask = 0
) noexcept {
    VkCommandBufferSubmitInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    info.pNext = nullptr;
    info.commandBuffer = cmd;
    info.deviceMask = deviceMask;
    return info;
}

/// Maps an opaque command buffer pointer to a Vulkan 1.3 VkCommandBufferSubmitInfo.
inline VkCommandBufferSubmitInfo toVkCommandBufferSubmitInfo(
    const void* cmd,
    u32 deviceMask = 0
) noexcept {
    return toVkCommandBufferSubmitInfo(toVkCommandBuffer(cmd), deviceMask);
}

/// Destination slices for zero-allocation VkSubmitInfo2 construction.
struct SubmitStorage {
    std::span<VkSemaphoreSubmitInfo> waitInfos{};
    std::span<VkCommandBufferSubmitInfo> commandInfos{};
    std::span<VkSemaphoreSubmitInfo> signalInfos{};
};

/// Fixed-capacity stack-allocated storage for zero-allocation translation without heap or arena overhead.
template <usize MaxWaits, usize MaxCmds, usize MaxSignals>
struct FixedSubmitStorage {
    std::array<VkSemaphoreSubmitInfo, MaxWaits> waitInfos{};
    std::array<VkCommandBufferSubmitInfo, MaxCmds> commandInfos{};
    std::array<VkSemaphoreSubmitInfo, MaxSignals> signalInfos{};

    LF_NODISCARD SubmitStorage asStorage() noexcept {
        return SubmitStorage{
            .waitInfos = waitInfos,
            .commandInfos = commandInfos,
            .signalInfos = signalInfos
        };
    }
};

/// Translates a LightFlow GpuSubmission into a native Vulkan 1.3 VkSubmitInfo2 structure
/// writing directly into caller-provided spans. Zero heap allocations.
inline VkSubmitInfo2 toVkSubmitInfo2(
    const GpuSubmission& submission,
    std::span<VkSemaphoreSubmitInfo> outWaits,
    std::span<VkCommandBufferSubmitInfo> outCmds,
    std::span<VkSemaphoreSubmitInfo> outSignals
) noexcept {
    const usize totalWaits = submission.waits.size() + submission.waitSyncPoints.size();
    const usize totalCmds = submission.commandBuffers.size();
    const usize totalSignals = submission.signals.size() + submission.signalSyncPoints.size();

    LF_ASSERT(outWaits.size() >= totalWaits && "outWaits span capacity is insufficient");
    LF_ASSERT(outCmds.size() >= totalCmds && "outCmds span capacity is insufficient");
    LF_ASSERT(outSignals.size() >= totalSignals && "outSignals span capacity is insufficient");

    const VkPipelineStageFlags2 waitDefault = (submission.defaultWaitStageMask != 0)
        ? submission.defaultWaitStageMask
        : getDefaultWaitStage(submission.queue);

    const VkPipelineStageFlags2 signalDefault = (submission.defaultSignalStageMask != 0)
        ? submission.defaultSignalStageMask
        : getDefaultSignalStage(submission.queue);

    // 1. Populate wait semaphores
    usize waitIdx = 0;
    for (const auto& waitDesc : submission.waits) {
        outWaits[waitIdx++] = toVkSemaphoreSubmitInfo(waitDesc, waitDefault);
    }
    for (const auto& sp : submission.waitSyncPoints) {
        outWaits[waitIdx++] = toVkSemaphoreSubmitInfo(sp, waitDefault);
    }

    // 2. Populate command buffers
    usize cmdIdx = 0;
    for (const void* cmdPtr : submission.commandBuffers) {
        outCmds[cmdIdx++] = toVkCommandBufferSubmitInfo(cmdPtr);
    }

    // 3. Populate signal semaphores
    usize sigIdx = 0;
    for (const auto& sigDesc : submission.signals) {
        outSignals[sigIdx++] = toVkSemaphoreSubmitInfo(sigDesc, signalDefault);
    }
    for (const auto& sp : submission.signalSyncPoints) {
        outSignals[sigIdx++] = toVkSemaphoreSubmitInfo(sp, signalDefault);
    }

    VkSubmitInfo2 submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submitInfo.pNext = nullptr;
    submitInfo.flags = static_cast<VkSubmitFlags>(submission.submitFlags);
    submitInfo.waitSemaphoreInfoCount = static_cast<u32>(totalWaits);
    submitInfo.pWaitSemaphoreInfos = totalWaits > 0 ? outWaits.data() : nullptr;
    submitInfo.commandBufferInfoCount = static_cast<u32>(totalCmds);
    submitInfo.pCommandBufferInfos = totalCmds > 0 ? outCmds.data() : nullptr;
    submitInfo.signalSemaphoreInfoCount = static_cast<u32>(totalSignals);
    submitInfo.pSignalSemaphoreInfos = totalSignals > 0 ? outSignals.data() : nullptr;

    return submitInfo;
}

/// Translates a GpuSubmission into a native VkSubmitInfo2 using caller-provided SubmitStorage.
inline VkSubmitInfo2 toVkSubmitInfo2(
    const GpuSubmission& submission,
    SubmitStorage storage
) noexcept {
    return toVkSubmitInfo2(submission, storage.waitInfos, storage.commandInfos, storage.signalInfos);
}

/// Translates a GpuSubmission into a native VkSubmitInfo2 allocating internal info arrays
/// directly from the provided SlabArena with zero malloc overhead.
inline VkSubmitInfo2 toVkSubmitInfo2(
    const GpuSubmission& submission,
    SlabArena& arena
) noexcept {
    const usize totalWaits = submission.waits.size() + submission.waitSyncPoints.size();
    const usize totalCmds = submission.commandBuffers.size();
    const usize totalSignals = submission.signals.size() + submission.signalSyncPoints.size();

    auto outWaits = arena.allocate_span<VkSemaphoreSubmitInfo>(totalWaits);
    auto outCmds = arena.allocate_span<VkCommandBufferSubmitInfo>(totalCmds);
    auto outSignals = arena.allocate_span<VkSemaphoreSubmitInfo>(totalSignals);

    return toVkSubmitInfo2(submission, outWaits, outCmds, outSignals);
}

/// Translates a batch of GpuSubmissions into a destination slice of VkSubmitInfo2,
/// allocating arrays from the thread's local SlabArena.
inline usize toVkSubmitInfo2Batch(
    std::span<const GpuSubmission> submissions,
    std::span<VkSubmitInfo2> outSubmits,
    SlabArena& arena
) noexcept {
    const usize count = std::min(submissions.size(), outSubmits.size());
    for (usize i = 0; i < count; ++i) {
        outSubmits[i] = toVkSubmitInfo2(submissions[i], arena);
    }
    return count;
}

/// Translates a batch of GpuSubmissions into a newly arena-allocated span of VkSubmitInfo2.
inline std::span<VkSubmitInfo2> toVkSubmitInfo2Batch(
    std::span<const GpuSubmission> submissions,
    SlabArena& arena
) noexcept {
    if (submissions.empty()) {
        return {};
    }
    auto outSubmits = arena.allocate_span<VkSubmitInfo2>(submissions.size());
    if (outSubmits.empty()) {
        return {};
    }
    toVkSubmitInfo2Batch(submissions, outSubmits, arena);
    return outSubmits;
}

} // namespace lf::vk
