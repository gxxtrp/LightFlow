# TaskGraph & Graph Primitives API Reference

The [`TaskGraph`](file:///Users/admin/Work/TEMP/task-scheduler/include/lightflow/task/task_graph.hpp) is LightFlow's core directed acyclic graph (DAG) representation. It manages task dependencies, condition branching, dynamic subflow generation, and frame execution state.

---

## Architectural Mechanics

### 1. Dual-State In-Degree Tracking
Traditional task graphs require traversing and resetting every node pointer before re-execution, or reallocating nodes every frame. LightFlow employs a **dual-state in-degree model**:
* **`initialInDegree`**: Computed once during graph construction (number of incoming dependency edges).
* **`inDegree` (`std::atomic<u32>`)**: Decremented atomically by predecessors as they complete execution.

When a predecessor finishes, it decrements each successor's `inDegree` via `fetch_sub(1, std::memory_order_acq_rel)`. The thread that observes the counter reach `0` is guaranteed to have unblocked the task and pushes it directly into its local Chase-Lev deque.

To reset the graph for the next frame, [`clear()`](#taskgraphclear) or [`prepareRun()`](#taskgraphpreparerun) simply copies `initialInDegree` back into `inDegree` in a single flat loop. **Reset is an instantaneous $O(N)$ memory write with zero pointer chasing and zero heap allocations.**

### 2. Mechanical Sympathy & Cacheline Layout
`TaskNode` is aligned to `alignas(64)`:
* **Hot Cacheline (Bytes 0–63)**: Contains fields accessed in the hot execution path: `initialInDegree`, `inDegree`, `type`, `domain`, `priority`, `successors` edge pointer, and the 48-byte Small Buffer Optimization (SBO) callable.
* **Cold Cacheline (Bytes 64–127)**: Contains debug name (`const char*`), condition branch state, subflow pointers, and cleanup destructors.

---

## Class Reference: `TaskGraph`

```cpp
namespace lf {
    class TaskGraph;
}
```

### Constructor & Destructor

```cpp
explicit TaskGraph(BlockPool* pool = nullptr);
~TaskGraph() noexcept;
```
* **Parameters**:
  * `pool`: Optional pointer to an external [`BlockPool`](file:///Users/admin/Work/TEMP/task-scheduler/docs/api/memory.md). If `nullptr`, defaults to `BlockPool::global()`.
* **Allocation Guarantee**: Zero heap allocations. All memory allocates from the arena slabs.
* **Complexity**: $O(1)$.

---

### Task Emplacement (`emplace`)

```cpp
template <typename F>
TaskHandle emplace(
    F&& callable,
    TaskDomain domain = TaskDomain::Worker,
    TaskPriority priority = TaskPriority::Normal
);

template <typename F>
TaskHandle emplace(
    const char* name,
    F&& callable,
    TaskDomain domain = TaskDomain::Worker,
    TaskPriority priority = TaskPriority::Normal
);
```
Creates a static task node in the graph.

* **Parameters**:
  * `name`: Optional debug string (zero-copy pointer storage, no allocations).
  * `callable`: Invocable object (`void()` or `void() noexcept`). Must fit within 48 bytes for inline SBO storage; larger callables allocate in the graph's `SlabArena`.
  * `domain`: Execution domain (`TaskDomain::Worker`, `TaskDomain::MainThread`, `TaskDomain::IO`).
  * `priority`: Priority level (`TaskPriority::Normal`, `TaskPriority::High`).
* **Returns**: A [`TaskHandle`](#class-reference-taskhandle) representing the created node.
* **Complexity**: $O(1)$ monotonic bump allocation.

#### Example
```cpp
lf::TaskGraph graph;

auto t1 = graph.emplace("CullEntities", []() noexcept {
    // Frustum cull entities
});

auto t2 = graph.emplace("UploadGeometry", []() noexcept {
    // Stream dynamic vertices
}, lf::TaskDomain::Worker, lf::TaskPriority::High);
```

---

### Dynamic Subflows (`emplaceSubflow`)

```cpp
template <typename F>
SubflowHandle emplaceSubflow(
    F&& callable,
    TaskDomain domain = TaskDomain::Worker,
    TaskPriority priority = TaskPriority::Normal
);

template <typename F>
SubflowHandle emplaceSubflow(
    const char* name,
    F&& callable,
    TaskDomain domain = TaskDomain::Worker,
    TaskPriority priority = TaskPriority::Normal
);
```
Spawns a dynamic child task graph at runtime from within a worker thread.

* **Callable Signature**: `void(lf::Subflow& childGraph) noexcept`
* **Execution Model**:
  1. The worker thread executing the subflow node invokes `callable(childGraph)`.
  2. The callable builds a dependency graph inside `childGraph`. All child nodes allocate wait-free from the worker thread's local 64 KB `SlabArena`.
  3. The subflow dynamically inserts an internal join barrier. Downstream successors of the subflow node will not unblock until all tasks inside the subflow complete.

#### Example
```cpp
auto bloomPass = graph.emplaceSubflow("DynamicBloom", [](lf::Subflow& sub) noexcept {
    auto downsample4x = sub.emplace([]() noexcept { /* ... */ });
    auto downsample8x = sub.emplace([]() noexcept { /* ... */ });
    auto blur         = sub.emplace([]() noexcept { /* ... */ });
    auto upsample     = sub.emplace([]() noexcept { /* ... */ });

    downsample4x.precede(downsample8x);
    downsample8x.precede(blur);
    blur.precede(upsample);
});
```

---

### Condition Nodes & Cascade Inactivation (`emplaceCondition`)

```cpp
template <typename F>
ConditionHandle emplaceCondition(
    F&& condition,
    TaskDomain domain = TaskDomain::Worker,
    TaskPriority priority = TaskPriority::Normal
);

template <typename F>
ConditionHandle emplaceCondition(
    const char* name,
    F&& condition,
    TaskDomain domain = TaskDomain::Worker,
    TaskPriority priority = TaskPriority::Normal
);
```
Creates a dynamic branch decision node.

* **Condition Callable**: Must return `int` representing the selected outgoing branch index (`0`, `1`, `2`, ...).
* **Cascade Inactivation**:
  When the condition executes and selects branch $B$, all outgoing edges matching branch index $B$ are unblocked as normal. **All unselected edges ($b \neq B$) are marked as skipped.**
  
  LightFlow's **Cascade Inactivation algorithm** recursively walks downstream tasks along skipped paths, decrementing their in-degrees with a skip flag. If a downstream join barrier's dependencies are all satisfied or skipped, the barrier unblocks immediately without ever pushing unselected tasks to worker queues. This guarantees **zero CPU cycles wasted on ghost tasks**.

#### Example
```cpp
auto checkShadowBudget = graph.emplaceCondition("CheckShadows", []() noexcept -> int {
    return (g_enableCascadedShadows) ? 0 : 1;
});

auto fullShadowPass = graph.emplace("RenderCascades", []() noexcept { /* ... */ });
auto simpleShadowPass = graph.emplace("RenderSimpleShadows", []() noexcept { /* ... */ });
auto composite = graph.emplace("CompositeLight", []() noexcept { /* ... */ });

// Branch 0 -> full shadows
checkShadowBudget.precede(fullShadowPass, 0);
// Branch 1 -> simple shadows
checkShadowBudget.precede(simpleShadowPass, 1);

// Both branch into composite
fullShadowPass.precede(composite);
simpleShadowPass.precede(composite);
```

---

### Frame Lifecycle: `clear` & `prepareRun`

```cpp
void clear() noexcept;
void prepareRun() noexcept;
```
* **`clear()`**: Rolls back the graph's bump allocator to zero. Deallocates all task nodes, dependency edges, and closures in $O(1)$. Slabs remain pooled in the `BlockPool` for the next frame.
* **`prepareRun()`**: Prepares an existing static graph for re-execution by copying `initialInDegree` into the atomic `inDegree` counter for all nodes. $O(N)$ flat loop, zero allocations.

---

## Class Reference: `TaskHandle`

A lightweight, trivially copyable 16-byte value handle pointing to a task node.

```cpp
namespace lf {
    class TaskHandle;
}
```

### Dependency Chaining Methods

```cpp
TaskHandle precede(TaskHandle other, int branch = -1) const noexcept;
TaskHandle succeed(TaskHandle other, int branch = -1) const noexcept;
```
* **`precede(other)`**: Configures `this` task to execute **before** `other`. Adds a directed edge `this -> other`.
* **`succeed(other)`**: Configures `this` task to execute **after** `other`. Adds a directed edge `other -> this`.
* **`branch`**: Optional branch index for condition nodes (defaults to `-1` for unconditional edges).

### Operator `>>` (Chaining Syntax)

```cpp
inline TaskHandle operator>>(TaskHandle lhs, TaskHandle rhs) noexcept {
    return lhs.precede(rhs);
}
```

#### Example
```cpp
// Fluent pipeline syntax:
initMesh >> computeCulling >> recordCommandBuffer >> presentFrame;
```

---

## Thread-Safety & Invariant Contract

| Operation | Calling Context | Thread-Safety Guarantee | Complexity |
| :--- | :--- | :--- | :---: |
| `graph.emplace()` | Main thread or Subflow | Single-thread per graph | $O(1)$ bump |
| `graph.clear()` | Frame start / teardown | Single-thread | $O(1)$ rollback |
| `handle.precede()` | Graph build phase | Single-thread | $O(1)$ bump |
| `TaskNode::execute()` | Worker execution | Lock-free, wait-free | Direct fn call |
| `fetch_sub` in-degree | Worker dependency resolution | Atomic release-acquire | $O(1)$ atomic |
