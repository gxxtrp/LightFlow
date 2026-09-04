#include <catch2/catch_test_macros.hpp>
#include <catch2/benchmark/catch_benchmark.hpp>
#include <lightflow/lightflow.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <future>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <queue>
#include <thread>
#include <vector>

#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__) || \
    (defined(__has_feature) && (__has_feature(thread_sanitizer) || __has_feature(address_sanitizer))) || \
    defined(LF_ENABLE_ASAN) || defined(LF_ENABLE_TSAN) || defined(LF_ENABLE_TRACY)
    #define LF_INSTRUMENTED_BUILD 1
#endif

// =============================================================================
// Global Heap Allocation Tracker
// =============================================================================

namespace lf::comparison {

struct AllocationTracker {
    static inline std::atomic<bool> s_active{false};
    static inline std::atomic<size_t> s_allocationCount{0};
    static inline std::atomic<size_t> s_deallocationCount{0};
    static inline std::atomic<size_t> s_allocatedBytes{0};

    static void reset() noexcept {
        s_allocationCount.store(0, std::memory_order_relaxed);
        s_deallocationCount.store(0, std::memory_order_relaxed);
        s_allocatedBytes.store(0, std::memory_order_relaxed);
    }

    static void enable() noexcept {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        s_active.store(true, std::memory_order_seq_cst);
    }

    static void disable() noexcept {
        s_active.store(false, std::memory_order_seq_cst);
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }

    static size_t allocations() noexcept {
        return s_allocationCount.load(std::memory_order_relaxed);
    }

    static size_t deallocations() noexcept {
        return s_deallocationCount.load(std::memory_order_relaxed);
    }

    static size_t bytes() noexcept {
        return s_allocatedBytes.load(std::memory_order_relaxed);
    }
};

} // namespace lf::comparison

void* operator new(std::size_t size) {
    if (lf::comparison::AllocationTracker::s_active.load(std::memory_order_relaxed)) {
        lf::comparison::AllocationTracker::s_allocationCount.fetch_add(1, std::memory_order_relaxed);
        lf::comparison::AllocationTracker::s_allocatedBytes.fetch_add(size, std::memory_order_relaxed);
    }
    void* p = std::malloc(size);
    if (p == nullptr) {
        std::abort();
    }
    return p;
}

void* operator new[](std::size_t size) {
    if (lf::comparison::AllocationTracker::s_active.load(std::memory_order_relaxed)) {
        lf::comparison::AllocationTracker::s_allocationCount.fetch_add(1, std::memory_order_relaxed);
        lf::comparison::AllocationTracker::s_allocatedBytes.fetch_add(size, std::memory_order_relaxed);
    }
    void* p = std::malloc(size);
    if (p == nullptr) {
        std::abort();
    }
    return p;
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    if (lf::comparison::AllocationTracker::s_active.load(std::memory_order_relaxed)) {
        lf::comparison::AllocationTracker::s_allocationCount.fetch_add(1, std::memory_order_relaxed);
        lf::comparison::AllocationTracker::s_allocatedBytes.fetch_add(size, std::memory_order_relaxed);
    }
    return std::malloc(size);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    if (lf::comparison::AllocationTracker::s_active.load(std::memory_order_relaxed)) {
        lf::comparison::AllocationTracker::s_allocationCount.fetch_add(1, std::memory_order_relaxed);
        lf::comparison::AllocationTracker::s_allocatedBytes.fetch_add(size, std::memory_order_relaxed);
    }
    return std::malloc(size);
}

void operator delete(void* p) noexcept {
    if (lf::comparison::AllocationTracker::s_active.load(std::memory_order_relaxed)) {
        lf::comparison::AllocationTracker::s_deallocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}

void operator delete[](void* p) noexcept {
    if (lf::comparison::AllocationTracker::s_active.load(std::memory_order_relaxed)) {
        lf::comparison::AllocationTracker::s_deallocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}

void operator delete(void* p, std::size_t) noexcept {
    if (lf::comparison::AllocationTracker::s_active.load(std::memory_order_relaxed)) {
        lf::comparison::AllocationTracker::s_deallocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}

void operator delete[](void* p, std::size_t) noexcept {
    if (lf::comparison::AllocationTracker::s_active.load(std::memory_order_relaxed)) {
        lf::comparison::AllocationTracker::s_deallocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}

void operator delete(void* p, const std::nothrow_t&) noexcept {
    if (lf::comparison::AllocationTracker::s_active.load(std::memory_order_relaxed)) {
        lf::comparison::AllocationTracker::s_deallocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}

void operator delete[](void* p, const std::nothrow_t&) noexcept {
    if (lf::comparison::AllocationTracker::s_active.load(std::memory_order_relaxed)) {
        lf::comparison::AllocationTracker::s_deallocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}

void operator delete(void* p, std::size_t, const std::nothrow_t&) noexcept {
    if (lf::comparison::AllocationTracker::s_active.load(std::memory_order_relaxed)) {
        lf::comparison::AllocationTracker::s_deallocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}

void operator delete[](void* p, std::size_t, const std::nothrow_t&) noexcept {
    if (lf::comparison::AllocationTracker::s_active.load(std::memory_order_relaxed)) {
        lf::comparison::AllocationTracker::s_deallocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}

// =============================================================================
// Classic Concurrency Baseline: Mutex + CondVar ThreadPool & Dynamic Task Graph
// =============================================================================

namespace lf::comparison {

class ClassicThreadPool {
public:
    explicit ClassicThreadPool(size_t threadCount) {
        workers_.reserve(threadCount);
        for (size_t i = 0; i < threadCount; ++i) {
            workers_.emplace_back([this]() {
                workerLoop();
            });
        }
    }

    ~ClassicThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            stop_.store(true, std::memory_order_relaxed);
        }
        cv_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    ClassicThreadPool(const ClassicThreadPool&) = delete;
    ClassicThreadPool& operator=(const ClassicThreadPool&) = delete;
    ClassicThreadPool(ClassicThreadPool&&) = delete;
    ClassicThreadPool& operator=(ClassicThreadPool&&) = delete;

    void enqueue(std::function<void()> task) {
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            tasks_.push(std::move(task));
        }
        cv_.notify_one();
    }

    size_t workerCount() const noexcept {
        return workers_.size();
    }

private:
    void workerLoop() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(queueMutex_);
                cv_.wait(lock, [this]() {
                    return stop_.load(std::memory_order_relaxed) || !tasks_.empty();
                });
                if (stop_.load(std::memory_order_relaxed) && tasks_.empty()) {
                    return;
                }
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            task();
        }
    }

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex queueMutex_;
    std::condition_variable cv_;
    std::atomic<bool> stop_{false};
};

class ClassicTaskGraph {
public:
    struct Node {
        std::function<void()> work;
        std::vector<Node*> successors;
        std::atomic<uint32_t> inDegree{0};
        uint32_t initialInDegree{0};
    };

    Node* emplace(std::function<void()> work) {
        auto node = std::make_unique<Node>();
        node->work = std::move(work);
        Node* ptr = node.get();
        nodes_.push_back(std::move(node));
        return ptr;
    }

    void addEdge(Node* from, Node* to) {
        from->successors.push_back(to);
        to->initialInDegree++;
    }

    void reset() {
        for (auto& node : nodes_) {
            node->inDegree.store(node->initialInDegree, std::memory_order_relaxed);
        }
    }

    size_t nodeCount() const noexcept {
        return nodes_.size();
    }

    void executeAndWait(ClassicThreadPool& pool) {
        struct ExecutionContext {
            ClassicThreadPool* poolPtr{nullptr};
            std::atomic<size_t> remainingTasks{0};
            std::mutex completionMutex;
            std::condition_variable completionCv;
            bool completed{false};
        };

        ExecutionContext ctx;
        ctx.poolPtr = &pool;
        ctx.remainingTasks.store(nodes_.size(), std::memory_order_relaxed);

        auto executeNode = [](auto self, ExecutionContext* c, Node* n) -> void {
            n->work();

            for (Node* succ : n->successors) {
                if (succ->inDegree.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    c->poolPtr->enqueue([self, c, succ]() {
                        self(self, c, succ);
                    });
                }
            }

            if (c->remainingTasks.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                std::unique_lock<std::mutex> lock(c->completionMutex);
                c->completed = true;
                c->completionCv.notify_one();
            }
        };

        // Dispatch all root tasks (initialInDegree == 0)
        for (auto& node : nodes_) {
            if (node->initialInDegree == 0) {
                Node* raw = node.get();
                pool.enqueue([&executeNode, &ctx, raw]() {
                    executeNode(executeNode, &ctx, raw);
                });
            }
        }

        // Wait for all tasks to complete
        std::unique_lock<std::mutex> lock(ctx.completionMutex);
        ctx.completionCv.wait(lock, [&ctx]() {
            return ctx.completed;
        });
    }

private:
    std::vector<std::unique_ptr<Node>> nodes_;
};

// =============================================================================
// Latency Statistics & Generously Spaced Comparison Formatter
// =============================================================================

using Clock = std::chrono::high_resolution_clock;
using DurationUs = std::chrono::duration<double, std::micro>;

struct LatencyStats {
    double minUs{0.0};
    double maxUs{0.0};
    double meanUs{0.0};
    double p50Us{0.0};
    double p95Us{0.0};
    double p99Us{0.0};

    static LatencyStats compute(std::vector<double>& samples) {
        if (samples.empty()) {
            return {};
        }
        std::sort(samples.begin(), samples.end());
        double sum = std::accumulate(samples.begin(), samples.end(), 0.0);
        double mean = sum / static_cast<double>(samples.size());
        double min = samples.front();
        double max = samples.back();

        auto percentile = [&](double p) -> double {
            double idx = p * static_cast<double>(samples.size() - 1);
            auto i = static_cast<size_t>(idx);
            double frac = idx - static_cast<double>(i);
            if (i + 1 < samples.size()) {
                return samples[i] * (1.0 - frac) + samples[i + 1] * frac;
            }
            return samples[i];
        };

        return LatencyStats{
            .minUs = min,
            .maxUs = max,
            .meanUs = mean,
            .p50Us = percentile(0.50),
            .p95Us = percentile(0.95),
            .p99Us = percentile(0.99)
        };
    }
};

void printComparisonTable(
    const char* benchmarkTitle,
    size_t taskCount,
    size_t iterations,
    size_t threadCount,
    const LatencyStats& lfStats,
    const LatencyStats& classicStats
) {
    double speedupMean = (lfStats.meanUs > 0.0) ? (classicStats.meanUs / lfStats.meanUs) : 1.0;
    double speedupP50 = (lfStats.p50Us > 0.0) ? (classicStats.p50Us / lfStats.p50Us) : 1.0;

    std::cout << "\n========================================================================================================================\n"
              << " Concurrency Comparison: " << benchmarkTitle << "\n"
              << " Tasks: " << taskCount
              << " | Iterations: " << iterations
              << " | Worker Threads: " << threadCount << "\n"
              << "========================================================================================================================\n"
              << std::left << std::setw(30) << "Framework"
              << std::right
              << std::setw(14) << "Min (us)"
              << std::setw(14) << "Mean (us)"
              << std::setw(14) << "P50 (us)"
              << std::setw(14) << "P95 (us)"
              << std::setw(14) << "Max (us)"
              << std::setw(18) << "Speedup"
              << "\n"
              << "------------------------------------------------------------------------------------------------------------------------\n";

    auto printRow = [](const char* name, const LatencyStats& s, const char* speedupStr) {
        std::cout << std::left << std::setw(30) << name
                  << std::right << std::fixed << std::setprecision(2)
                  << std::setw(14) << s.minUs
                  << std::setw(14) << s.meanUs
                  << std::setw(14) << s.p50Us
                  << std::setw(14) << s.p95Us
                  << std::setw(14) << s.maxUs
                  << std::setw(18) << speedupStr
                  << "\n";
    };

    char lfSpeedupBuf[32];
    std::snprintf(lfSpeedupBuf, sizeof(lfSpeedupBuf), "%.2fx", speedupMean);

    printRow("LightFlow (Lock-Free)", lfStats, lfSpeedupBuf);
    printRow("Classic ThreadPool", classicStats, "1.00x (ref)");

    std::cout << "------------------------------------------------------------------------------------------------------------------------\n"
              << " >>> LightFlow is " << std::fixed << std::setprecision(2) << speedupMean
              << "x faster on average (P50 speedup: " << speedupP50 << "x) <<<\n"
              << "========================================================================================================================\n";
}

} // namespace lf::comparison

// =============================================================================
// TEST SUITE: Comparative Concurrency Benchmarks & Extreme Stress Tests
// =============================================================================

TEST_CASE("Comparison: 50,000-Task Wavefront (Fan-Out & Fan-In)", "[comparison][wavefront][stress]") {
    using namespace lf::comparison;

    constexpr size_t TASK_COUNT = 50000;
    const size_t THREAD_COUNT = std::max<size_t>(2, std::thread::hardware_concurrency());
#if defined(LF_INSTRUMENTED_BUILD)
    constexpr size_t ITERATIONS = 3;
#else
    constexpr size_t ITERATIONS = 10;
#endif

    // -------------------------------------------------------------------------
    // 1. LightFlow Implementation
    // -------------------------------------------------------------------------
    lf::SchedulerConfig lfConfig{
        .workerCount = static_cast<lf::u32>(THREAD_COUNT),
        .initialDequeCapacity = 65536
    };
    lf::TaskScheduler lfScheduler(lfConfig);

    lf::TaskGraph lfGraph;
    std::atomic<size_t> lfCounter{0};

    lf::TaskHandle lfRoot = lfGraph.emplace([&lfCounter]() noexcept {
        lfCounter.fetch_add(1, std::memory_order_relaxed);
    });

    std::vector<lf::TaskHandle> lfLeaves;
    lfLeaves.reserve(TASK_COUNT - 2);

    for (size_t i = 0; i < TASK_COUNT - 2; ++i) {
        lf::TaskHandle leaf = lfGraph.emplace([&lfCounter]() noexcept {
            lfCounter.fetch_add(1, std::memory_order_relaxed);
        });
        lfRoot.precede(leaf);
        lfLeaves.push_back(leaf);
    }

    lf::TaskHandle lfJoin = lfGraph.emplace([&lfCounter]() noexcept {
        lfCounter.fetch_add(1, std::memory_order_relaxed);
    });

    for (lf::TaskHandle leaf : lfLeaves) {
        leaf.precede(lfJoin);
    }

    // Warm-up LightFlow
    lfScheduler.runAndWait(lfGraph);
    REQUIRE(lfCounter.load() == TASK_COUNT);

    std::vector<double> lfSamples;
    lfSamples.reserve(ITERATIONS);

    for (size_t it = 0; it < ITERATIONS; ++it) {
        lfCounter.store(0, std::memory_order_relaxed);

        auto t0 = Clock::now();
        lfScheduler.runAndWait(lfGraph);
        auto t1 = Clock::now();

        lfSamples.push_back(std::chrono::duration_cast<DurationUs>(t1 - t0).count());
    }
    REQUIRE(lfCounter.load() == TASK_COUNT);

    // -------------------------------------------------------------------------
    // 2. Classic Baseline Implementation
    // -------------------------------------------------------------------------
    ClassicThreadPool classicPool(THREAD_COUNT);
    ClassicTaskGraph classicGraph;
    std::atomic<size_t> classicCounter{0};

    auto* classicRoot = classicGraph.emplace([&classicCounter]() {
        classicCounter.fetch_add(1, std::memory_order_relaxed);
    });

    std::vector<ClassicTaskGraph::Node*> classicLeaves;
    classicLeaves.reserve(TASK_COUNT - 2);

    for (size_t i = 0; i < TASK_COUNT - 2; ++i) {
        auto* leaf = classicGraph.emplace([&classicCounter]() {
            classicCounter.fetch_add(1, std::memory_order_relaxed);
        });
        classicGraph.addEdge(classicRoot, leaf);
        classicLeaves.push_back(leaf);
    }

    auto* classicJoin = classicGraph.emplace([&classicCounter]() {
        classicCounter.fetch_add(1, std::memory_order_relaxed);
    });

    for (auto* leaf : classicLeaves) {
        classicGraph.addEdge(leaf, classicJoin);
    }

    // Warm-up Classic
    classicGraph.reset();
    classicGraph.executeAndWait(classicPool);
    REQUIRE(classicCounter.load() == TASK_COUNT);

    std::vector<double> classicSamples;
    classicSamples.reserve(ITERATIONS);

    for (size_t it = 0; it < ITERATIONS; ++it) {
        classicCounter.store(0, std::memory_order_relaxed);
        classicGraph.reset();

        auto t0 = Clock::now();
        classicGraph.executeAndWait(classicPool);
        auto t1 = Clock::now();

        classicSamples.push_back(std::chrono::duration_cast<DurationUs>(t1 - t0).count());
    }
    REQUIRE(classicCounter.load() == TASK_COUNT);

    // -------------------------------------------------------------------------
    // 3. Evaluation & Output
    // -------------------------------------------------------------------------
    auto lfStats = LatencyStats::compute(lfSamples);
    auto classicStats = LatencyStats::compute(classicSamples);

    printComparisonTable(
        "50,000-Task Wavefront (Fan-Out & Fan-In)",
        TASK_COUNT,
        ITERATIONS,
        THREAD_COUNT,
        lfStats,
        classicStats
    );

    CHECK(lfStats.meanUs < classicStats.meanUs);
}

TEST_CASE("Comparison: 50,000-Task Multi-Stage Pipeline (10x5,000 Tasks)", "[comparison][pipeline][stress]") {
    using namespace lf::comparison;

    constexpr size_t STAGES = 10;
    constexpr size_t TASKS_PER_STAGE = 5000;
    constexpr size_t TOTAL_TASKS = STAGES * TASKS_PER_STAGE;
    const size_t THREAD_COUNT = std::max<size_t>(2, std::thread::hardware_concurrency());
#if defined(LF_INSTRUMENTED_BUILD)
    constexpr size_t ITERATIONS = 3;
#else
    constexpr size_t ITERATIONS = 10;
#endif

    // -------------------------------------------------------------------------
    // 1. LightFlow Multi-Stage Pipeline
    // -------------------------------------------------------------------------
    lf::SchedulerConfig lfConfig{
        .workerCount = static_cast<lf::u32>(THREAD_COUNT),
        .initialDequeCapacity = 16384
    };
    lf::TaskScheduler lfScheduler(lfConfig);

    lf::TaskGraph lfGraph;
    std::atomic<size_t> lfCounter{0};

    std::vector<std::vector<lf::TaskHandle>> lfStages(STAGES);
    for (size_t s = 0; s < STAGES; ++s) {
        lfStages[s].reserve(TASKS_PER_STAGE);
        for (size_t t = 0; t < TASKS_PER_STAGE; ++t) {
            lf::TaskHandle node = lfGraph.emplace([&lfCounter]() noexcept {
                lfCounter.fetch_add(1, std::memory_order_relaxed);
            });
            lfStages[s].push_back(node);
        }
    }

    for (size_t s = 1; s < STAGES; ++s) {
        lf::TaskHandle barrier = lfGraph.emplace([]() noexcept {});
        for (lf::TaskHandle prev : lfStages[s - 1]) {
            prev.precede(barrier);
        }
        for (lf::TaskHandle next : lfStages[s]) {
            barrier.precede(next);
        }
    }

    // Warm-up LightFlow
    lfScheduler.runAndWait(lfGraph);
    REQUIRE(lfCounter.load() == TOTAL_TASKS);

    std::vector<double> lfSamples;
    lfSamples.reserve(ITERATIONS);

    for (size_t it = 0; it < ITERATIONS; ++it) {
        lfCounter.store(0, std::memory_order_relaxed);

        auto t0 = Clock::now();
        lfScheduler.runAndWait(lfGraph);
        auto t1 = Clock::now();

        lfSamples.push_back(std::chrono::duration_cast<DurationUs>(t1 - t0).count());
    }
    REQUIRE(lfCounter.load() == TOTAL_TASKS);

    // -------------------------------------------------------------------------
    // 2. Classic Multi-Stage Pipeline
    // -------------------------------------------------------------------------
    ClassicThreadPool classicPool(THREAD_COUNT);
    ClassicTaskGraph classicGraph;
    std::atomic<size_t> classicCounter{0};

    std::vector<std::vector<ClassicTaskGraph::Node*>> classicStages(STAGES);
    for (size_t s = 0; s < STAGES; ++s) {
        classicStages[s].reserve(TASKS_PER_STAGE);
        for (size_t t = 0; t < TASKS_PER_STAGE; ++t) {
            auto* node = classicGraph.emplace([&classicCounter]() {
                classicCounter.fetch_add(1, std::memory_order_relaxed);
            });
            classicStages[s].push_back(node);
        }
    }

    for (size_t s = 1; s < STAGES; ++s) {
        auto* barrier = classicGraph.emplace([]() {});
        for (auto* prev : classicStages[s - 1]) {
            classicGraph.addEdge(prev, barrier);
        }
        for (auto* next : classicStages[s]) {
            classicGraph.addEdge(barrier, next);
        }
    }

    // Warm-up Classic
    classicGraph.reset();
    classicGraph.executeAndWait(classicPool);
    REQUIRE(classicCounter.load() == TOTAL_TASKS);

    std::vector<double> classicSamples;
    classicSamples.reserve(ITERATIONS);

    for (size_t it = 0; it < ITERATIONS; ++it) {
        classicCounter.store(0, std::memory_order_relaxed);
        classicGraph.reset();

        auto t0 = Clock::now();
        classicGraph.executeAndWait(classicPool);
        auto t1 = Clock::now();

        classicSamples.push_back(std::chrono::duration_cast<DurationUs>(t1 - t0).count());
    }
    REQUIRE(classicCounter.load() == TOTAL_TASKS);

    // -------------------------------------------------------------------------
    // 3. Evaluation & Output
    // -------------------------------------------------------------------------
    auto lfStats = LatencyStats::compute(lfSamples);
    auto classicStats = LatencyStats::compute(classicSamples);

    printComparisonTable(
        "50,000-Task Multi-Stage Pipeline (10x5,000 Tasks)",
        TOTAL_TASKS,
        ITERATIONS,
        THREAD_COUNT,
        lfStats,
        classicStats
    );

    CHECK(lfStats.meanUs < classicStats.meanUs);
}

TEST_CASE("Comparison: 5,000,000-Item Parallel For Workload", "[comparison][parallel_for][stress]") {
    using namespace lf::comparison;

    constexpr size_t ITEM_COUNT = 5000000;
    constexpr size_t BATCH_SIZE = 2048;
    const size_t THREAD_COUNT = std::max<size_t>(2, std::thread::hardware_concurrency());
#if defined(LF_INSTRUMENTED_BUILD)
    constexpr size_t ITERATIONS = 3;
#else
    constexpr size_t ITERATIONS = 10;
#endif

    std::vector<uint32_t> lfData(ITEM_COUNT, 0);
    std::vector<uint32_t> classicData(ITEM_COUNT, 0);

    // -------------------------------------------------------------------------
    // 1. LightFlow Parallel For
    // -------------------------------------------------------------------------
    lf::SchedulerConfig lfConfig{
        .workerCount = static_cast<lf::u32>(THREAD_COUNT),
        .initialDequeCapacity = 4096
    };
    lf::TaskScheduler lfScheduler(lfConfig);

    lf::TaskGraph lfGraph;
    lfGraph.parallelFor(ITEM_COUNT, BATCH_SIZE, [&lfData](size_t idx) noexcept {
        lfData[idx] = static_cast<uint32_t>(idx * 3 + 1);
    });

    // Warm-up
    lfScheduler.runAndWait(lfGraph);

    std::vector<double> lfSamples;
    lfSamples.reserve(ITERATIONS);

    for (size_t it = 0; it < ITERATIONS; ++it) {
        auto t0 = Clock::now();
        lfScheduler.runAndWait(lfGraph);
        auto t1 = Clock::now();

        lfSamples.push_back(std::chrono::duration_cast<DurationUs>(t1 - t0).count());
    }

    // -------------------------------------------------------------------------
    // 2. Classic ThreadPool Chunked Loop
    // -------------------------------------------------------------------------
    ClassicThreadPool classicPool(THREAD_COUNT);
    std::vector<double> classicSamples;
    classicSamples.reserve(ITERATIONS);

    const size_t chunks = (ITEM_COUNT + BATCH_SIZE - 1) / BATCH_SIZE;

    for (size_t it = 0; it < ITERATIONS; ++it) {
        std::atomic<size_t> remainingChunks{chunks};
        std::mutex mtx;
        std::condition_variable cv;
        bool done = false;

        auto t0 = Clock::now();
        for (size_t c = 0; c < chunks; ++c) {
            size_t start = c * BATCH_SIZE;
            size_t end = std::min(start + BATCH_SIZE, ITEM_COUNT);

            classicPool.enqueue([&classicData, &remainingChunks, &mtx, &cv, &done, start, end]() {
                for (size_t i = start; i < end; ++i) {
                    classicData[i] = static_cast<uint32_t>(i * 3 + 1);
                }
                if (remainingChunks.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    std::unique_lock<std::mutex> lock(mtx);
                    done = true;
                    cv.notify_one();
                }
            });
        }

        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [&done]() { return done; });
        auto t1 = Clock::now();

        classicSamples.push_back(std::chrono::duration_cast<DurationUs>(t1 - t0).count());
    }

    bool dataMatches = (lfData == classicData);
    REQUIRE(dataMatches);

    auto lfStats = LatencyStats::compute(lfSamples);
    auto classicStats = LatencyStats::compute(classicSamples);

    printComparisonTable(
        "5,000,000-Item Parallel For Workload",
        ITEM_COUNT,
        ITERATIONS,
        THREAD_COUNT,
        lfStats,
        classicStats
    );
}

TEST_CASE("Comparison: Steady-State Heap Allocation Profile (100 Frames, 10,000 Tasks/Frame, 1,000,000 Total Tasks)", "[comparison][allocation][stress]") {
    using namespace lf::comparison;

    constexpr size_t TASK_COUNT = 10000;
    constexpr size_t FRAME_COUNT = 100;
    constexpr size_t TOTAL_EXECUTIONS = TASK_COUNT * FRAME_COUNT;
    const size_t THREAD_COUNT = std::max<size_t>(2, std::thread::hardware_concurrency());

    // -------------------------------------------------------------------------
    // 1. LightFlow Steady-State Allocations
    // -------------------------------------------------------------------------
    lf::SchedulerConfig lfConfig{
        .workerCount = static_cast<lf::u32>(THREAD_COUNT),
        .initialDequeCapacity = 16384
    };
    lf::TaskScheduler lfScheduler(lfConfig);

    lf::TaskGraph lfGraph;
    std::atomic<size_t> lfCounter{0};

    lf::TaskHandle lfRoot = lfGraph.emplace([&lfCounter]() noexcept {
        lfCounter.fetch_add(1, std::memory_order_relaxed);
    });

    std::vector<lf::TaskHandle> lfLeaves;
    lfLeaves.reserve(TASK_COUNT - 2);

    for (size_t i = 0; i < TASK_COUNT - 2; ++i) {
        lf::TaskHandle leaf = lfGraph.emplace([&lfCounter]() noexcept {
            lfCounter.fetch_add(1, std::memory_order_relaxed);
        });
        lfRoot.precede(leaf);
        lfLeaves.push_back(leaf);
    }

    lf::TaskHandle lfJoin = lfGraph.emplace([&lfCounter]() noexcept {
        lfCounter.fetch_add(1, std::memory_order_relaxed);
    });

    for (lf::TaskHandle leaf : lfLeaves) {
        leaf.precede(lfJoin);
    }

    // Warm-up LightFlow
    for (int w = 0; w < 3; ++w) {
        lfScheduler.runAndWait(lfGraph);
    }
    REQUIRE(lfCounter.load() == TASK_COUNT * 3);

    // Track steady-state frame loop allocations
    AllocationTracker::reset();
    AllocationTracker::enable();

    for (size_t f = 0; f < FRAME_COUNT; ++f) {
        lfScheduler.runAndWait(lfGraph);
    }

    AllocationTracker::disable();
    size_t lfSteadyStateAllocs = AllocationTracker::allocations();
    size_t lfSteadyStateBytes = AllocationTracker::bytes();

    REQUIRE(lfCounter.load() == TASK_COUNT * (FRAME_COUNT + 3));

    // -------------------------------------------------------------------------
    // 2. Classic Baseline Steady-State Allocations
    // -------------------------------------------------------------------------
    ClassicThreadPool classicPool(THREAD_COUNT);
    ClassicTaskGraph classicGraph;
    std::atomic<size_t> classicCounter{0};

    auto* classicRoot = classicGraph.emplace([&classicCounter]() {
        classicCounter.fetch_add(1, std::memory_order_relaxed);
    });

    std::vector<ClassicTaskGraph::Node*> classicLeaves;
    classicLeaves.reserve(TASK_COUNT - 2);

    for (size_t i = 0; i < TASK_COUNT - 2; ++i) {
        auto* leaf = classicGraph.emplace([&classicCounter]() {
            classicCounter.fetch_add(1, std::memory_order_relaxed);
        });
        classicGraph.addEdge(classicRoot, leaf);
        classicLeaves.push_back(leaf);
    }

    auto* classicJoin = classicGraph.emplace([&classicCounter]() {
        classicCounter.fetch_add(1, std::memory_order_relaxed);
    });

    for (auto* leaf : classicLeaves) {
        classicGraph.addEdge(leaf, classicJoin);
    }

    // Warm-up Classic
    for (int w = 0; w < 3; ++w) {
        classicGraph.reset();
        classicGraph.executeAndWait(classicPool);
    }
    REQUIRE(classicCounter.load() == TASK_COUNT * 3);

    // Track steady-state frame loop allocations
    AllocationTracker::reset();
    AllocationTracker::enable();

    for (size_t f = 0; f < FRAME_COUNT; ++f) {
        classicGraph.reset();
        classicGraph.executeAndWait(classicPool);
    }

    AllocationTracker::disable();
    size_t classicSteadyStateAllocs = AllocationTracker::allocations();
    size_t classicSteadyStateBytes = AllocationTracker::bytes();

    REQUIRE(classicCounter.load() == TASK_COUNT * (FRAME_COUNT + 3));

    // -------------------------------------------------------------------------
    // 3. Print Allocation Comparison Table
    // -------------------------------------------------------------------------
    std::cout << "\n========================================================================================================================\n"
              << " Steady-State Memory Allocation Profile: " << FRAME_COUNT << " Frames (10,000 Tasks/Frame, Total Tasks: " << TOTAL_EXECUTIONS << ")\n"
              << "========================================================================================================================\n"
              << std::left << std::setw(30) << "Framework"
              << std::right
              << std::setw(22) << "Allocs / Frame"
              << std::setw(28) << "Total Allocs (100f)"
              << std::setw(28) << "Total Bytes (100f)"
              << "\n"
              << "------------------------------------------------------------------------------------------------------------------------\n";

    std::cout << std::left << std::setw(30) << "LightFlow (Lock-Free)"
              << std::right
              << std::setw(22) << (lfSteadyStateAllocs / FRAME_COUNT)
              << std::setw(28) << lfSteadyStateAllocs
              << std::setw(26) << lfSteadyStateBytes << " B\n";

    double classicMb = static_cast<double>(classicSteadyStateBytes) / (1024.0 * 1024.0);
    char classicBytesBuf[64];
    std::snprintf(classicBytesBuf, sizeof(classicBytesBuf), "%zu B (%.2f MB)", classicSteadyStateBytes, classicMb);

    std::cout << std::left << std::setw(30) << "Classic ThreadPool"
              << std::right
              << std::setw(22) << (classicSteadyStateAllocs / FRAME_COUNT)
              << std::setw(28) << classicSteadyStateAllocs
              << std::setw(28) << classicBytesBuf << "\n";

    std::cout << "------------------------------------------------------------------------------------------------------------------------\n"
              << " >>> LightFlow Steady-State Zero-Allocation Guarantee: "
              << (lfSteadyStateAllocs == 0 ? "CONFIRMED (0 ALLOCATIONS)" : "FAILED")
              << " <<<\n"
              << "========================================================================================================================\n\n";

#if !defined(LF_INSTRUMENTED_BUILD)
    REQUIRE(lfSteadyStateAllocs == 0);
    CHECK(classicSteadyStateAllocs > 0);
#endif
}

TEST_CASE("Comparison: 200-Task Fan-Out Burst (LightFlow vs Classic vs std::async)", "[comparison][async]") {
    using namespace lf::comparison;

    constexpr size_t TASK_COUNT = 200;
    const size_t THREAD_COUNT = std::max<size_t>(2, std::thread::hardware_concurrency());
#if defined(LF_INSTRUMENTED_BUILD)
    constexpr size_t ITERATIONS = 5;
#else
    constexpr size_t ITERATIONS = 20;
#endif

    // 1. LightFlow Implementation
    lf::SchedulerConfig lfConfig{
        .workerCount = static_cast<lf::u32>(THREAD_COUNT),
        .initialDequeCapacity = 512
    };
    lf::TaskScheduler lfScheduler(lfConfig);

    lf::TaskGraph lfGraph;
    std::atomic<size_t> lfCounter{0};

    lf::TaskHandle lfRoot = lfGraph.emplace([&lfCounter]() noexcept {
        lfCounter.fetch_add(1, std::memory_order_relaxed);
    });
    lf::TaskHandle lfJoin = lfGraph.emplace([&lfCounter]() noexcept {
        lfCounter.fetch_add(1, std::memory_order_relaxed);
    });

    for (size_t i = 0; i < TASK_COUNT - 2; ++i) {
        lf::TaskHandle leaf = lfGraph.emplace([&lfCounter]() noexcept {
            lfCounter.fetch_add(1, std::memory_order_relaxed);
        });
        lfRoot.precede(leaf);
        leaf.precede(lfJoin);
    }

    lfScheduler.runAndWait(lfGraph);
    REQUIRE(lfCounter.load() == TASK_COUNT);

    std::vector<double> lfSamples;
    lfSamples.reserve(ITERATIONS);

    for (size_t it = 0; it < ITERATIONS; ++it) {
        lfCounter.store(0, std::memory_order_relaxed);
        auto t0 = Clock::now();
        lfScheduler.runAndWait(lfGraph);
        auto t1 = Clock::now();
        lfSamples.push_back(std::chrono::duration_cast<DurationUs>(t1 - t0).count());
    }
    REQUIRE(lfCounter.load() == TASK_COUNT);

    // 2. Classic ThreadPool Implementation
    ClassicThreadPool classicPool(THREAD_COUNT);
    ClassicTaskGraph classicGraph;
    std::atomic<size_t> classicCounter{0};

    auto* classicRoot = classicGraph.emplace([&classicCounter]() {
        classicCounter.fetch_add(1, std::memory_order_relaxed);
    });
    auto* classicJoin = classicGraph.emplace([&classicCounter]() {
        classicCounter.fetch_add(1, std::memory_order_relaxed);
    });

    for (size_t i = 0; i < TASK_COUNT - 2; ++i) {
        auto* leaf = classicGraph.emplace([&classicCounter]() {
            classicCounter.fetch_add(1, std::memory_order_relaxed);
        });
        classicGraph.addEdge(classicRoot, leaf);
        classicGraph.addEdge(leaf, classicJoin);
    }

    classicGraph.reset();
    classicGraph.executeAndWait(classicPool);
    REQUIRE(classicCounter.load() == TASK_COUNT);

    std::vector<double> classicSamples;
    classicSamples.reserve(ITERATIONS);

    for (size_t it = 0; it < ITERATIONS; ++it) {
        classicCounter.store(0, std::memory_order_relaxed);
        classicGraph.reset();
        auto t0 = Clock::now();
        classicGraph.executeAndWait(classicPool);
        auto t1 = Clock::now();
        classicSamples.push_back(std::chrono::duration_cast<DurationUs>(t1 - t0).count());
    }
    REQUIRE(classicCounter.load() == TASK_COUNT);

    // 3. std::async (launch::async) Baseline
    std::atomic<size_t> asyncCounter{0};
    std::vector<double> asyncSamples;
    asyncSamples.reserve(ITERATIONS);

    for (size_t it = 0; it < ITERATIONS; ++it) {
        asyncCounter.store(0, std::memory_order_relaxed);
        auto t0 = Clock::now();

        std::vector<std::future<void>> futures;
        futures.reserve(TASK_COUNT);
        for (size_t i = 0; i < TASK_COUNT; ++i) {
            futures.push_back(std::async(std::launch::async, [&asyncCounter]() {
                asyncCounter.fetch_add(1, std::memory_order_relaxed);
            }));
        }
        for (auto& f : futures) {
            f.get();
        }
        auto t1 = Clock::now();
        asyncSamples.push_back(std::chrono::duration_cast<DurationUs>(t1 - t0).count());
    }
    REQUIRE(asyncCounter.load() == TASK_COUNT);

    auto lfStats = LatencyStats::compute(lfSamples);
    auto classicStats = LatencyStats::compute(classicSamples);
    auto asyncStats = LatencyStats::compute(asyncSamples);

    std::cout << "\n========================================================================================================================\n"
              << " Concurrency Comparison: 200-Task Fan-Out Burst (LightFlow vs Classic vs std::async)\n"
              << " Tasks: " << TASK_COUNT
              << " | Iterations: " << ITERATIONS
              << " | Worker Threads: " << THREAD_COUNT << "\n"
              << "========================================================================================================================\n"
              << std::left << std::setw(30) << "Framework"
              << std::right
              << std::setw(14) << "Min (us)"
              << std::setw(14) << "Mean (us)"
              << std::setw(14) << "P50 (us)"
              << std::setw(14) << "P95 (us)"
              << std::setw(14) << "Max (us)"
              << std::setw(18) << "Speedup"
              << "\n"
              << "------------------------------------------------------------------------------------------------------------------------\n";

    auto printRow = [](const char* name, const LatencyStats& s, const char* speedupStr) {
        std::cout << std::left << std::setw(30) << name
                  << std::right << std::fixed << std::setprecision(2)
                  << std::setw(14) << s.minUs
                  << std::setw(14) << s.meanUs
                  << std::setw(14) << s.p50Us
                  << std::setw(14) << s.p95Us
                  << std::setw(14) << s.maxUs
                  << std::setw(18) << speedupStr
                  << "\n";
    };

    char lfSpeedup[32];
    std::snprintf(lfSpeedup, sizeof(lfSpeedup), "%.2fx", (classicStats.meanUs / lfStats.meanUs));
    char asyncSpeedup[32];
    std::snprintf(asyncSpeedup, sizeof(asyncSpeedup), "%.2fx slower", (asyncStats.meanUs / lfStats.meanUs));

    printRow("LightFlow (Lock-Free)", lfStats, lfSpeedup);
    printRow("Classic ThreadPool", classicStats, "1.00x (ref)");
    printRow("std::async (launch::async)", asyncStats, asyncSpeedup);

    std::cout << "------------------------------------------------------------------------------------------------------------------------\n"
              << " >>> LightFlow is " << std::fixed << std::setprecision(2)
              << (asyncStats.meanUs / lfStats.meanUs)
              << "x faster than std::async (" << (classicStats.meanUs / lfStats.meanUs)
              << "x faster than Classic ThreadPool) <<<\n"
              << "========================================================================================================================\n\n";

    CHECK(lfStats.meanUs < classicStats.meanUs);
    CHECK(classicStats.meanUs < asyncStats.meanUs);
}

// =============================================================================
// ULTIMATE BREAKING POINT: 1,000,000 Parallel Tasks Under Extreme Load
// =============================================================================

TEST_CASE("Ultimate Breaking Point: 1,000,000 Parallel Tasks", "[comparison][breaking_point][stress]") {
    using namespace lf::comparison;

    constexpr size_t TASK_COUNT = 1000000;
    constexpr size_t NUM_BATCHES = 100;
    constexpr size_t BATCH_SIZE = TASK_COUNT / NUM_BATCHES; // 10,000 tasks / batch
    const size_t THREAD_COUNT = std::max<size_t>(2, std::thread::hardware_concurrency());
#if defined(LF_INSTRUMENTED_BUILD)
    constexpr size_t ITERATIONS = 2;
#else
    constexpr size_t ITERATIONS = 5;
#endif

    // -------------------------------------------------------------------------
    // 1. LightFlow Implementation (1,000,000 Parallel Tasks)
    // -------------------------------------------------------------------------
    lf::SchedulerConfig lfConfig{
        .workerCount = static_cast<lf::u32>(THREAD_COUNT),
        .initialDequeCapacity = 131072
    };
    lf::TaskScheduler lfScheduler(lfConfig);

    lf::TaskGraph lfGraph;
    std::atomic<size_t> lfCounter{0};

    lf::TaskHandle lfRoot = lfGraph.emplace([&lfCounter]() noexcept {
        lfCounter.fetch_add(1, std::memory_order_relaxed);
    });

    lf::TaskHandle lfJoin = lfGraph.emplace([&lfCounter]() noexcept {
        lfCounter.fetch_add(1, std::memory_order_relaxed);
    });

    std::vector<lf::TaskHandle> lfCollectors;
    lfCollectors.reserve(NUM_BATCHES);
    for (size_t b = 0; b < NUM_BATCHES; ++b) {
        lf::TaskHandle collector = lfGraph.emplace([&lfCounter]() noexcept {
            lfCounter.fetch_add(1, std::memory_order_relaxed);
        });
        collector.precede(lfJoin);
        lfCollectors.push_back(collector);
    }

    for (size_t i = 0; i < TASK_COUNT; ++i) {
        lf::TaskHandle child = lfGraph.emplace([&lfCounter]() noexcept {
            lfCounter.fetch_add(1, std::memory_order_relaxed);
        });
        lfRoot.precede(child);
        child.precede(lfCollectors[i / BATCH_SIZE]);
    }

    const size_t TOTAL_GRAPH_NODES = TASK_COUNT + NUM_BATCHES + 2;

    // Warm-up LightFlow
    lfScheduler.runAndWait(lfGraph);
    REQUIRE(lfCounter.load() == TOTAL_GRAPH_NODES);

    std::vector<double> lfSamples;
    lfSamples.reserve(ITERATIONS);

    for (size_t it = 0; it < ITERATIONS; ++it) {
        lfCounter.store(0, std::memory_order_relaxed);
        auto t0 = Clock::now();
        lfScheduler.runAndWait(lfGraph);
        auto t1 = Clock::now();
        lfSamples.push_back(std::chrono::duration_cast<DurationUs>(t1 - t0).count());
    }
    REQUIRE(lfCounter.load() == TOTAL_GRAPH_NODES);

    // -------------------------------------------------------------------------
    // 2. Classic Baseline Implementation (1,000,000 Parallel Tasks)
    // -------------------------------------------------------------------------
    ClassicThreadPool classicPool(THREAD_COUNT);
    ClassicTaskGraph classicGraph;
    std::atomic<size_t> classicCounter{0};

    auto* classicRoot = classicGraph.emplace([&classicCounter]() {
        classicCounter.fetch_add(1, std::memory_order_relaxed);
    });

    auto* classicJoin = classicGraph.emplace([&classicCounter]() {
        classicCounter.fetch_add(1, std::memory_order_relaxed);
    });

    std::vector<ClassicTaskGraph::Node*> classicCollectors;
    classicCollectors.reserve(NUM_BATCHES);
    for (size_t b = 0; b < NUM_BATCHES; ++b) {
        auto* collector = classicGraph.emplace([&classicCounter]() {
            classicCounter.fetch_add(1, std::memory_order_relaxed);
        });
        classicGraph.addEdge(collector, classicJoin);
        classicCollectors.push_back(collector);
    }

    for (size_t i = 0; i < TASK_COUNT; ++i) {
        auto* child = classicGraph.emplace([&classicCounter]() {
            classicCounter.fetch_add(1, std::memory_order_relaxed);
        });
        classicGraph.addEdge(classicRoot, child);
        classicGraph.addEdge(child, classicCollectors[i / BATCH_SIZE]);
    }

    // Warm-up Classic
    classicGraph.reset();
    classicGraph.executeAndWait(classicPool);
    REQUIRE(classicCounter.load() == TOTAL_GRAPH_NODES);

    std::vector<double> classicSamples;
    classicSamples.reserve(ITERATIONS);

    for (size_t it = 0; it < ITERATIONS; ++it) {
        classicCounter.store(0, std::memory_order_relaxed);
        classicGraph.reset();
        auto t0 = Clock::now();
        classicGraph.executeAndWait(classicPool);
        auto t1 = Clock::now();
        classicSamples.push_back(std::chrono::duration_cast<DurationUs>(t1 - t0).count());
    }
    REQUIRE(classicCounter.load() == TOTAL_GRAPH_NODES);

    // -------------------------------------------------------------------------
    // 3. Evaluation & Output
    // -------------------------------------------------------------------------
    auto lfStats = LatencyStats::compute(lfSamples);
    auto classicStats = LatencyStats::compute(classicSamples);

    printComparisonTable(
        "Ultimate Breaking Point: 1,000,000 Parallel Tasks",
        TASK_COUNT,
        ITERATIONS,
        THREAD_COUNT,
        lfStats,
        classicStats
    );

    double lfThroughput = (static_cast<double>(TASK_COUNT) / (lfStats.meanUs / 1'000'000.0));
    double classicThroughput = (static_cast<double>(TASK_COUNT) / (classicStats.meanUs / 1'000'000.0));

    std::cout << " Throughput Comparison:\n"
              << " - LightFlow (Lock-Free): " << std::fixed << std::setprecision(0)
              << lfThroughput << " tasks/sec (" << std::setprecision(2) << (lfThroughput / 1'000'000.0) << " M tasks/sec)\n"
              << " - Classic ThreadPool:   " << std::fixed << std::setprecision(0)
              << classicThroughput << " tasks/sec (" << std::setprecision(2) << (classicThroughput / 1'000'000.0) << " M tasks/sec)\n"
              << "========================================================================================================================\n\n";

    CHECK(lfStats.meanUs < classicStats.meanUs);
}
