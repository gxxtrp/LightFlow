#pragma once

#include <cstdint>

// Binary-compatible Vulkan 1.3 Synchronization2 definitions for host environments
// where the Vulkan SDK (<vulkan/vulkan.h>) is not installed (e.g. macOS CI).

#ifndef VULKAN_H_

#ifndef VKAPI_ATTR
#define VKAPI_ATTR
#endif
#ifndef VKAPI_CALL
#define VKAPI_CALL
#endif
#ifndef VKAPI_PTR
#define VKAPI_PTR
#endif

typedef uint32_t VkFlags;
typedef uint64_t VkFlags64;
typedef uint32_t VkStructureType;

#ifndef VK_DEFINE_HANDLE
#define VK_DEFINE_HANDLE(object) typedef struct object##_T* object;
#endif

#ifndef VK_DEFINE_NON_DISPATCHABLE_HANDLE
#define VK_DEFINE_NON_DISPATCHABLE_HANDLE(object) typedef struct object##_T* object;
#endif

VK_DEFINE_HANDLE(VkCommandBuffer)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkSemaphore)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkFence)
VK_DEFINE_HANDLE(VkQueue)
VK_DEFINE_HANDLE(VkDevice)

typedef VkFlags VkSubmitFlags;
typedef VkFlags VkSemaphoreSubmitFlags;
typedef VkFlags VkCommandBufferSubmitFlags;
typedef VkFlags VkQueueFlags;
typedef VkFlags64 VkPipelineStageFlags2;
typedef VkFlags64 VkAccessFlags2;

// Structure Types
inline constexpr VkStructureType VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO = 1000314000;
inline constexpr VkStructureType VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO_KHR = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
inline constexpr VkStructureType VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO = 1000314001;
inline constexpr VkStructureType VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO_KHR = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
inline constexpr VkStructureType VK_STRUCTURE_TYPE_SUBMIT_INFO_2 = 1000314004;
inline constexpr VkStructureType VK_STRUCTURE_TYPE_SUBMIT_INFO_2_KHR = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;

// Queue Flags
inline constexpr VkQueueFlags VK_QUEUE_GRAPHICS_BIT = 0x00000001;
inline constexpr VkQueueFlags VK_QUEUE_COMPUTE_BIT  = 0x00000002;
inline constexpr VkQueueFlags VK_QUEUE_TRANSFER_BIT = 0x00000004;
inline constexpr VkQueueFlags VK_QUEUE_SPARSE_BINDING_BIT = 0x00000008;
inline constexpr VkQueueFlags VK_QUEUE_PROTECTED_BIT = 0x00000010;

// Submit Flags
inline constexpr VkSubmitFlags VK_SUBMIT_PROTECTED_BIT = 0x00000001;
inline constexpr VkSubmitFlags VK_SUBMIT_PROTECTED_BIT_KHR = VK_SUBMIT_PROTECTED_BIT;

// Pipeline Stage Flags 2
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_NONE = 0ULL;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_NONE_KHR = 0ULL;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT = 0x00000001ULL;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT_KHR = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT = 0x00000002ULL;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT_KHR = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT = 0x00000004ULL;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT_KHR = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT = 0x00000008ULL;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT_KHR = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT = 0x00000010ULL;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT_KHR = VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT = 0x00000020ULL;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT_KHR = VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT = 0x00000040ULL;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT_KHR = VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT = 0x00000080ULL;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT_KHR = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT = 0x00000100ULL;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT_KHR = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT = 0x00000200ULL;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT_KHR = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT = 0x00000400ULL;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT_KHR = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT = 0x00000800ULL;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT_KHR = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_TRANSFER_BIT = 0x00001000ULL;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_TRANSFER_BIT_KHR = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT = 0x00002000ULL;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT_KHR = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_HOST_BIT = 0x00004000ULL;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_HOST_BIT_KHR = VK_PIPELINE_STAGE_2_HOST_BIT;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT = 0x00008000ULL;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT_KHR = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT = 0x00010000ULL;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT_KHR = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_COPY_BIT = 0x100000000ULL;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_COPY_BIT_KHR = VK_PIPELINE_STAGE_2_COPY_BIT;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_RESOLVE_BIT = 0x200000000ULL;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_RESOLVE_BIT_KHR = VK_PIPELINE_STAGE_2_RESOLVE_BIT;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_BLIT_BIT = 0x400000000ULL;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_BLIT_BIT_KHR = VK_PIPELINE_STAGE_2_BLIT_BIT;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_CLEAR_BIT = 0x800000000ULL;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_CLEAR_BIT_KHR = VK_PIPELINE_STAGE_2_CLEAR_BIT;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT = 0x1000000000ULL;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT_KHR = VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT = 0x2000000000ULL;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT_KHR = VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT = 0x4000000000ULL;
inline constexpr VkPipelineStageFlags2 VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT_KHR = VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT;

// Synchronization2 Structs
typedef struct VkSemaphoreSubmitInfo {
    VkStructureType sType;
    const void* pNext;
    VkSemaphore semaphore;
    uint64_t value;
    VkPipelineStageFlags2 stageMask;
    uint32_t deviceIndex;
} VkSemaphoreSubmitInfo;

typedef VkSemaphoreSubmitInfo VkSemaphoreSubmitInfoKHR;

typedef struct VkCommandBufferSubmitInfo {
    VkStructureType sType;
    const void* pNext;
    VkCommandBuffer commandBuffer;
    uint32_t deviceMask;
} VkCommandBufferSubmitInfo;

typedef VkCommandBufferSubmitInfo VkCommandBufferSubmitInfoKHR;

typedef struct VkSubmitInfo2 {
    VkStructureType sType;
    const void* pNext;
    VkSubmitFlags flags;
    uint32_t waitSemaphoreInfoCount;
    const VkSemaphoreSubmitInfo* pWaitSemaphoreInfos;
    uint32_t commandBufferInfoCount;
    const VkCommandBufferSubmitInfo* pCommandBufferInfos;
    uint32_t signalSemaphoreInfoCount;
    const VkSemaphoreSubmitInfo* pSignalSemaphoreInfos;
} VkSubmitInfo2;

typedef VkSubmitInfo2 VkSubmitInfo2KHR;

#endif // VULKAN_H_
