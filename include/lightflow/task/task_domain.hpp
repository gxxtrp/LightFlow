#pragma once

#include <lightflow/core/types.hpp>

#include <cstdint>

namespace lf {

/// Task execution priority levels.
enum class TaskPriority : u8 {
    High = 0,    ///< Frame-critical rendering commands & GPU submissions
    Normal = 1   ///< General CPU compute, asset loading, & background streaming
};

inline constexpr usize TASK_PRIORITY_COUNT = 2;

/// Task execution domain routing target.
enum class TaskDomain : u8 {
    Worker = 0,     ///< Standard multi-threaded work-stealing pool
    MainThread = 1, ///< Pushed to MPSC queue, drained explicitly via drainMainThreadTasks()
    IO = 2          ///< Routed to background IO worker pool
};

/// High-level archetype of a task node.
enum class TaskType : u8 {
    Static = 0,           ///< Standard CPU compute callable
    ParallelForChunk = 1, ///< Data-parallel loop chunk
    Subflow = 2,          ///< Dynamic child graph generated on worker thread
    Condition = 3,        ///< Conditional branch evaluation with cascade inactivation
    GpuSubmit = 4         ///< Heterogeneous GPU queue batch submission
};

/// Lifecycle execution state of a task node.
enum class TaskState : u8 {
    Pending = 0,   ///< Awaiting predecessor unblocking
    Executing = 1, ///< Currently running on a worker or caller thread
    Completed = 2, ///< Execution completed; successors unblocked
    Skipped = 3,   ///< Inactivated by conditional branching cascade
    Timeout = 4    ///< Watchdog timeout triggered; cancelled without execution
};

/// Execution status code returned by scheduler operations or queried from tasks.
enum class Status : u8 {
    Success = 0,    ///< Successful execution
    GpuTimeout = 1, ///< GPU timeline watchdog timeout triggered (driver TDR or hang)
    Error = 2       ///< Internal execution error
};

} // namespace lf
