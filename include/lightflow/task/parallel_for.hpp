#pragma once

#include <lightflow/core/types.hpp>
#include <lightflow/task/task_domain.hpp>
#include <lightflow/task/task_node.hpp>
#include <lightflow/task/task_handle.hpp>

#include <concepts>
#include <span>
#include <type_traits>
#include <utility>

namespace lf {

/// Encapsulates the partition range and chunk metadata for a parallel-for batch.
struct ChunkRange {
    usize start{0};
    usize end{0};
    usize chunkIndex{0};
    usize totalChunks{0};

    LF_NODISCARD constexpr usize size() const noexcept { return end - start; }
    LF_NODISCARD constexpr bool empty() const noexcept { return start >= end; }
};

/// Strongly typed wrapper representing a chunk index.
struct ChunkIndex {
    usize index{0};

    constexpr explicit ChunkIndex(usize idx) noexcept : index(idx) {}
    LF_NODISCARD explicit constexpr operator usize() const noexcept { return index; }
};

/// Tag type for explicitly dispatching chunk index invocation.
struct ByChunkTag {};
inline constexpr ByChunkTag ByChunk{};

// =============================================================================
// Callable Concepts
// =============================================================================

template <typename F>
concept RangeInvocableWithChunkIdx = requires(std::decay_t<F>& f, usize start, usize end, usize chunkIdx) {
    f(start, end, chunkIdx);
};

template <typename F>
concept RangeInvocable = requires(std::decay_t<F>& f, usize start, usize end) {
    f(start, end);
} && !RangeInvocableWithChunkIdx<F>;

template <typename F>
concept ChunkRangeInvocable = requires(std::decay_t<F>& f, const ChunkRange& range) {
    f(range);
};

template <typename F>
concept ItemInvocable = requires(std::decay_t<F>& f, usize idx) {
    f(idx);
} && !RangeInvocable<F> && !RangeInvocableWithChunkIdx<F> && !ChunkRangeInvocable<F>;

template <typename F>
concept ChunkIndexInvocable = requires(std::decay_t<F>& f, ChunkIndex idx) {
    f(idx);
} && !ItemInvocable<F>;

template <typename F, typename T>
concept SpanChunkInvocable = requires(std::decay_t<F>& f, std::span<T> chunk) {
    f(chunk);
};

template <typename F, typename T>
concept SpanItemWithIndexInvocable = requires(std::decay_t<F>& f, T& item, usize index) {
    f(item, index);
} && !SpanChunkInvocable<F, T>;

template <typename F, typename T>
concept SpanItemInvocable = requires(std::decay_t<F>& f, T& item) {
    f(item);
} && !SpanChunkInvocable<F, T> && !SpanItemWithIndexInvocable<F, T>;

// =============================================================================
// ParallelForHandle
// =============================================================================

/// Handle returned by TaskGraph::parallelFor representing the parallel-for loop.
/// Inherits TaskHandle so it transparently participates in linear (>>), fan-out, and fan-in chaining.
class ParallelForHandle : public TaskHandle {
public:
    constexpr ParallelForHandle() noexcept = default;

    constexpr ParallelForHandle(TaskNode* entry, TaskNode* exit, usize chunkCount, std::span<TaskNode*> chunks = {}) noexcept
        : TaskHandle(entry, exit), m_chunkCount(chunkCount), m_chunks(chunks) {}

    LF_NODISCARD constexpr usize chunkCount() const noexcept { return m_chunkCount; }
    LF_NODISCARD constexpr std::span<TaskNode*> chunks() const noexcept { return m_chunks; }

    /// Sets the execution domain on the entry, exit, and all chunk tasks.
    ParallelForHandle domain(TaskDomain d) const noexcept {
        TaskHandle::domain(d);
        for (TaskNode* chunk : m_chunks) {
            if (chunk != nullptr) {
                chunk->domain = d;
            }
        }
        return *this;
    }

    /// Sets the task priority on the entry, exit, and all chunk tasks.
    ParallelForHandle priority(TaskPriority p) const noexcept {
        TaskHandle::priority(p);
        for (TaskNode* chunk : m_chunks) {
            if (chunk != nullptr) {
                chunk->priority = p;
            }
        }
        return *this;
    }

private:
    usize m_chunkCount{0};
    std::span<TaskNode*> m_chunks{};
};

} // namespace lf
