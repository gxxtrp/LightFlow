#pragma once

#include <lightflow/core/types.hpp>
#include <lightflow/task/task_domain.hpp>
#include <lightflow/task/task_node.hpp>
#include <lightflow/gpu/timeline_sync_point.hpp>

#include <span>
#include <tuple>
#include <utility>

namespace lf {

class TaskGraph;
class BranchHandle;

/// Lightweight, value-type handle referencing a TaskNode within a TaskGraph.
/// Provides fluent C++23 chaining operators (precede, succeed, >>, std::tie fan-out/fan-in).
class TaskHandle {
public:
    constexpr TaskHandle() noexcept = default;
    constexpr explicit TaskHandle(TaskNode* node) noexcept : m_entry(node), m_exit(node) {}
    constexpr TaskHandle(TaskNode* entry, TaskNode* exit) noexcept : m_entry(entry), m_exit(exit) {}

    LF_NODISCARD constexpr TaskNode* node() const noexcept { return m_entry; }
    LF_NODISCARD constexpr TaskNode* entryNode() const noexcept { return m_entry; }
    LF_NODISCARD constexpr TaskNode* exitNode() const noexcept { return m_exit; }
    LF_NODISCARD constexpr TaskNode* operator->() const noexcept { return m_entry; }
    LF_NODISCARD constexpr explicit operator bool() const noexcept { return m_entry != nullptr; }
    LF_NODISCARD constexpr bool isCompound() const noexcept { return m_entry != m_exit; }

    LF_NODISCARD const char* name() const noexcept {
        return m_entry != nullptr ? m_entry->name : nullptr;
    }

    LF_NODISCARD TaskDomain domain() const noexcept {
        return m_entry != nullptr ? m_entry->domain : TaskDomain::Worker;
    }

    LF_NODISCARD TaskPriority priority() const noexcept {
        return m_entry != nullptr ? m_entry->priority : TaskPriority::Normal;
    }

    /// Fluent builder methods:
    TaskHandle domain(TaskDomain d) const noexcept {
        if (m_entry != nullptr) {
            m_entry->domain = d;
        }
        if (m_exit != nullptr && m_exit != m_entry) {
            m_exit->domain = d;
        }
        return *this;
    }

    TaskHandle priority(TaskPriority p) const noexcept {
        if (m_entry != nullptr) {
            m_entry->priority = p;
        }
        if (m_exit != nullptr && m_exit != m_entry) {
            m_exit->priority = p;
        }
        return *this;
    }

    TaskHandle name(const char* n) const noexcept {
        if (m_entry != nullptr) {
            m_entry->name = n;
        }
        if (m_exit != nullptr && m_exit != m_entry) {
            m_exit->name = n;
        }
        return *this;
    }

    /// Declares that this task waits on a GPU timeline synchronization barrier before execution.
    TaskHandle waits(const TimelineSyncPoint& syncPoint) const noexcept {
        if (m_entry != nullptr) {
            m_entry->waits(syncPoint);
        }
        return *this;
    }

    /// Queries the current execution status of this task node.
    LF_NODISCARD Status status() const noexcept {
        if (m_entry == nullptr) {
            return Status::Error;
        }
        if (m_entry->state.load(std::memory_order_relaxed) == TaskState::Timeout) {
            return Status::GpuTimeout;
        }
        return Status::Success;
    }

    // --- Dependency Chaining ---

    /// Declares that this task must complete before other starts (*this -> other).
    TaskHandle precede(TaskHandle other) const noexcept;

    /// Declares that this task must run after other finishes (other -> *this).
    TaskHandle succeed(TaskHandle other) const noexcept {
        other.precede(*this);
        return *this;
    }

    /// Declares multiple downstream successors for this task.
    template <typename... Tasks>
    TaskHandle precede(TaskHandle first, Tasks... rest) const noexcept {
        precede(first);
        (precede(rest), ...);
        return *this;
    }

    /// Declares multiple downstream successors from a span.
    TaskHandle precede(std::span<const TaskHandle> targets) const noexcept {
        for (const auto& target : targets) {
            precede(target);
        }
        return *this;
    }

    /// Declares multiple upstream predecessors from a span.
    TaskHandle succeed(std::span<const TaskHandle> sources) const noexcept {
        for (const auto& source : sources) {
            source.precede(*this);
        }
        return *this;
    }

    LF_NODISCARD BranchHandle to(int branchIndex) const noexcept;

    friend constexpr bool operator==(TaskHandle lhs, TaskHandle rhs) noexcept {
        return lhs.m_entry == rhs.m_entry && lhs.m_exit == rhs.m_exit;
    }

private:
    TaskNode* m_entry{nullptr};
    TaskNode* m_exit{nullptr};
};

/// Lightweight handle referencing a condition node with an associated branch index.
class BranchHandle {
public:
    constexpr BranchHandle() noexcept = default;
    constexpr BranchHandle(TaskNode* conditionNode, int branchIndex) noexcept
        : m_node(conditionNode), m_branch(branchIndex) {}

    LF_NODISCARD constexpr TaskNode* node() const noexcept { return m_node; }
    LF_NODISCARD constexpr int branch() const noexcept { return m_branch; }

    TaskHandle precede(TaskHandle other) const noexcept;

    template <typename... Tasks>
    BranchHandle precede(TaskHandle first, Tasks... rest) const noexcept {
        precede(first);
        (precede(rest), ...);
        return *this;
    }

    BranchHandle precede(std::span<const TaskHandle> targets) const noexcept {
        for (const auto& target : targets) {
            precede(target);
        }
        return *this;
    }

private:
    TaskNode* m_node{nullptr};
    int m_branch{-1};
};

inline BranchHandle TaskHandle::to(int branchIndex) const noexcept {
    return BranchHandle{m_exit, branchIndex};
}

/// Specialized TaskHandle representing a dynamic condition evaluation node.
class ConditionHandle : public TaskHandle {
public:
    using TaskHandle::TaskHandle;
    constexpr explicit ConditionHandle(TaskHandle handle) noexcept : TaskHandle(handle) {}
    constexpr explicit ConditionHandle(TaskNode* node) noexcept : TaskHandle(node) {}
};

// --- Fluent C++23 Operators ---

/// Linear chaining operator: `taskA >> taskB` declares `taskA -> taskB` and returns `taskB`.
/// Allows intuitive chaining: `taskA >> taskB >> taskC >> taskD`.
inline TaskHandle operator>>(TaskHandle lhs, TaskHandle rhs) noexcept {
    lhs.precede(rhs);
    return rhs;
}

/// Conditional branching operator: `cond.to(0) >> taskB` declares conditional dependency.
inline TaskHandle operator>>(BranchHandle lhs, TaskHandle rhs) noexcept {
    lhs.precede(rhs);
    return rhs;
}

/// Conditional fan-out operator: `cond.to(0) >> std::tie(b, c)`
template <typename... Ts>
inline BranchHandle operator>>(BranchHandle lhs, const std::tuple<Ts...>& targets) noexcept {
    std::apply([&](const auto&... target) {
        (lhs.precede(target), ...);
    }, targets);
    return lhs;
}

/// Fan-out operator: `taskA >> std::tie(taskB, taskC)` declares dependencies from taskA to each target.
template <typename... Ts>
inline TaskHandle operator>>(TaskHandle lhs, const std::tuple<Ts...>& targets) noexcept {
    std::apply([&](const auto&... target) {
        (lhs.precede(target), ...);
    }, targets);
    return lhs;
}

/// Fan-in operator: `std::tie(taskA, taskB) >> taskC` declares dependencies from each source to taskC.
template <typename... Ts>
inline TaskHandle operator>>(const std::tuple<Ts...>& sources, TaskHandle target) noexcept {
    std::apply([&](const auto&... source) {
        (source.precede(target), ...);
    }, sources);
    return target;
}

} // namespace lf
