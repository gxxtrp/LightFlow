#pragma once

#include <lightflow/core/types.hpp>
#include <lightflow/task/task_domain.hpp>
#include <lightflow/task/move_only_task.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

namespace lf {

class TaskGraph;
struct TaskNode;
struct SuccessorEdge;
struct TimelineSyncPoint;

/// Function pointer type for task node execution.
using TaskExecuteFn = void (*)(TaskNode*) noexcept;

/// Intrusive singly-linked edge structure connecting a task node to downstream successors.
/// Allocated from the graph's SlabArena bump allocator with zero steady-state heap overhead.
struct SuccessorEdge {
    TaskNode* target{nullptr};
    SuccessorEdge* next{nullptr};
    int branch{-1};
};

/// Copyable/moveable wrapper around std::atomic enabling TaskNode aggregate semantics
/// and std::vector compatibility without manual user-declared constructors.
template <typename T>
struct AtomicWrapper {
    std::atomic<T> value;

    constexpr AtomicWrapper() noexcept : value(T{}) {}
    constexpr explicit AtomicWrapper(T val) noexcept : value(val) {}

    AtomicWrapper(const AtomicWrapper& other) noexcept
        : value(other.value.load(std::memory_order_relaxed)) {}

    AtomicWrapper(AtomicWrapper&& other) noexcept
        : value(other.value.load(std::memory_order_relaxed)) {}

    AtomicWrapper& operator=(const AtomicWrapper& other) noexcept {
        if (this != &other) {
            value.store(other.value.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
        return *this;
    }

    AtomicWrapper& operator=(AtomicWrapper&& other) noexcept {
        if (this != &other) {
            value.store(other.value.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
        return *this;
    }

    T load(std::memory_order order = std::memory_order_seq_cst) const noexcept {
        return value.load(order);
    }

    void store(T desired, std::memory_order order = std::memory_order_seq_cst) noexcept {
        value.store(desired, order);
    }

    T fetch_sub(T arg, std::memory_order order = std::memory_order_seq_cst) noexcept {
        return value.fetch_sub(arg, order);
    }

    T fetch_add(T arg, std::memory_order order = std::memory_order_seq_cst) noexcept {
        return value.fetch_add(arg, order);
    }

    bool compare_exchange_weak(T& expected, T desired, std::memory_order success, std::memory_order failure) noexcept {
        return value.compare_exchange_weak(expected, desired, success, failure);
    }

    operator T() const noexcept {
        return load(std::memory_order_relaxed);
    }
};

/// High-performance cacheline-aligned (128-byte) stackless task execution node.
/// Designed as a C++23 aggregate with hot/cold cacheline splitting:
/// - Cacheline 1 (64 bytes): Hot scheduling & atomic dependency tracking:
///   id, priority, domain, type, state, inDegree, initialInDegree, and 6 hot pointers.
/// - Cacheline 2 (64 bytes): Inline 48-byte SBO callable storage, destroyFn, and name.
struct alignas(lf::CACHELINE_SIZE) TaskNode {
    // =========================================================================
    // Cacheline 1: Hot scheduling & atomic dependency tracking (64 bytes)
    // =========================================================================
    u32 id{0};
    TaskPriority priority{TaskPriority::Normal};
    TaskDomain domain{TaskDomain::Worker};
    TaskType type{TaskType::Static};
    AtomicWrapper<TaskState> state{TaskState::Pending};

    AtomicWrapper<u32> inDegree{0};
    u32 initialInDegree{0};

    void* userData{nullptr};
    TaskExecuteFn executeFn{nullptr};     ///< Hot execution entry point
    TaskNode* next{nullptr};              ///< Intrusive pointer for scheduler MPMC/MPSC queues
    SuccessorEdge* successors{nullptr};   ///< Singly-linked list of downstream dependencies
    TaskGraph* graph{nullptr};            ///< Pointer to owning TaskGraph (if any)
    TaskNode* nextInGraph{nullptr};       ///< Intrusive traversal pointer for TaskGraph

    // =========================================================================
    // Cacheline 2: Executable callable & cold metadata (64 bytes)
    // =========================================================================
    static constexpr usize INLINE_STORAGE_BYTES = 48;
    alignas(16) std::byte inlineStorage[INLINE_STORAGE_BYTES]{};
    void (*destroyFn)(TaskNode*) noexcept {nullptr};
    const char* name{nullptr};            ///< Cold debug / Tracy profiler identifier

    /// Invokes the stored callable, sets the completed state, and unblocks downstream successors.
    void execute() noexcept;

    /// Iterates successors, atomically decrements their inDegree, and schedules unblocked tasks.
    void unblockSuccessors() noexcept;

    /// Decrements dependency counter from an active completed predecessor.
    void decrementActive(TaskGraph* g) noexcept;

    /// Decrements dependency counter from an unselected/skipped predecessor.
    void decrementSkipped(TaskGraph* g) noexcept;

    /// Marks this node as Skipped, notifies graph, and cascades decrementSkipped to successors.
    void cascadeSkip(TaskGraph* g) noexcept;

    /// Marks this node as waiting on a GPU timeline synchronization barrier.
    TaskNode* waits(const TimelineSyncPoint& syncPoint) noexcept;

    /// Marks this node as timed out, notifies graph, and cascades skip to successors.
    void handleGpuTimeout(TaskGraph* g) noexcept;

    /// Destroys any active closure inside inlineStorage or arena overflow.
    void destroy() noexcept {
        if (destroyFn != nullptr) {
            destroyFn(this);
            destroyFn = nullptr;
        }
        executeFn = nullptr;
    }

    /// Stores a callable inside the node's 48-byte SBO buffer or in the provided SlabArena.
    template <typename F>
        requires (!std::is_same_v<std::decay_t<F>, TaskNode> && std::is_invocable_v<std::decay_t<F>>)
    void setCallable(F&& callable, SlabArena* arena = nullptr) {
        destroy();
        using DecayedF = std::decay_t<F>;

        if constexpr (sizeof(DecayedF) <= INLINE_STORAGE_BYTES && alignof(DecayedF) <= 16) {
            ::new (static_cast<void*>(inlineStorage)) DecayedF(std::forward<F>(callable));

            executeFn = [](TaskNode* node) noexcept {
                (*static_cast<DecayedF*>(static_cast<void*>(node->inlineStorage)))();
            };

            destroyFn = [](TaskNode* node) noexcept {
                static_cast<DecayedF*>(static_cast<void*>(node->inlineStorage))->~DecayedF();
            };
        } else {
            LF_ASSERT(arena != nullptr && "Callable exceeds 48-byte SBO buffer but no SlabArena was provided");
            void* mem = arena->allocate(sizeof(DecayedF), alignof(DecayedF));
            DecayedF* obj = ::new (mem) DecayedF(std::forward<F>(callable));
            *reinterpret_cast<DecayedF**>(inlineStorage) = obj;

            executeFn = [](TaskNode* node) noexcept {
                auto* objPtr = *reinterpret_cast<DecayedF**>(node->inlineStorage);
                (*objPtr)();
            };

            destroyFn = [](TaskNode* node) noexcept {
                auto* objPtr = *reinterpret_cast<DecayedF**>(node->inlineStorage);
                if (objPtr != nullptr) {
                    objPtr->~DecayedF();
                }
            };
        }
    }

    /// Stores a condition callable returning an integer branch index.
    template <typename F>
        requires (!std::is_same_v<std::decay_t<F>, TaskNode> && std::is_invocable_v<std::decay_t<F>>)
    void setConditionCallable(F&& callable, SlabArena* arena = nullptr) {
        type = TaskType::Condition;
        destroy();
        using DecayedF = std::decay_t<F>;

        if constexpr (sizeof(DecayedF) <= INLINE_STORAGE_BYTES && alignof(DecayedF) <= 16) {
            ::new (static_cast<void*>(inlineStorage)) DecayedF(std::forward<F>(callable));

            executeFn = [](TaskNode* node) noexcept {
                int branch = static_cast<int>((*static_cast<DecayedF*>(static_cast<void*>(node->inlineStorage)))());
                node->userData = reinterpret_cast<void*>(static_cast<intptr_t>(branch));
            };

            destroyFn = [](TaskNode* node) noexcept {
                static_cast<DecayedF*>(static_cast<void*>(node->inlineStorage))->~DecayedF();
            };
        } else {
            LF_ASSERT(arena != nullptr && "Callable exceeds 48-byte SBO buffer but no SlabArena was provided");
            void* mem = arena->allocate(sizeof(DecayedF), alignof(DecayedF));
            DecayedF* obj = ::new (mem) DecayedF(std::forward<F>(callable));
            *reinterpret_cast<DecayedF**>(inlineStorage) = obj;

            executeFn = [](TaskNode* node) noexcept {
                auto* objPtr = *reinterpret_cast<DecayedF**>(node->inlineStorage);
                int branch = static_cast<int>((*objPtr)());
                node->userData = reinterpret_cast<void*>(static_cast<intptr_t>(branch));
            };

            destroyFn = [](TaskNode* node) noexcept {
                auto* objPtr = *reinterpret_cast<DecayedF**>(node->inlineStorage);
                if (objPtr != nullptr) {
                    objPtr->~DecayedF();
                }
            };
        }
    }
};

static_assert(sizeof(TaskNode) == 128, "TaskNode must be exactly 128 bytes (2 cache lines)");
static_assert(alignof(TaskNode) == 64, "TaskNode must be aligned to 64 bytes");
static_assert(offsetof(TaskNode, inlineStorage) == 64, "inlineStorage must start at Cacheline 2 boundary (offset 64)");

} // namespace lf
