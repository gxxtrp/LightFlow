#include <lightflow/lightflow.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <queue>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>
#else
    #include <sys/mman.h>
    #include <unistd.h>
#endif

// =============================================================================
// Global Heap Allocation Tracker
// =============================================================================

namespace lf::bench {

inline void* bench_alloc(lf::usize bytes, lf::usize alignment, void* /*user_data*/) noexcept {
#if defined(_WIN32)
    (void)alignment;
    return ::VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    #if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
        #define MAP_ANONYMOUS MAP_ANON
    #endif
    const long sys_page = ::sysconf(_SC_PAGESIZE);
    const lf::usize page_size = (sys_page > 0) ? static_cast<lf::usize>(sys_page) : 4096;
    if (alignment <= page_size) {
        void* ptr = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        return (ptr == MAP_FAILED) ? nullptr : ptr;
    }
    const lf::usize total_reserve = bytes + alignment;
    void* raw = ::mmap(nullptr, total_reserve, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) {
        return nullptr;
    }
    const auto raw_addr = reinterpret_cast<std::uintptr_t>(raw);
    const auto aligned_addr = (raw_addr + alignment - 1) & ~(alignment - 1);
    const lf::usize prefix = aligned_addr - raw_addr;
    const lf::usize suffix = total_reserve - prefix - bytes;
    if (prefix > 0) {
        ::munmap(raw, prefix);
    }
    if (suffix > 0) {
        ::munmap(reinterpret_cast<void*>(aligned_addr + bytes), suffix);
    }
    return reinterpret_cast<void*>(aligned_addr);
#endif
}

inline void bench_free(void* ptr, lf::usize bytes, lf::usize /*alignment*/, void* /*user_data*/) noexcept {
    if (ptr == nullptr) return;
#if defined(_WIN32)
    (void)bytes;
    ::VirtualFree(ptr, 0, MEM_RELEASE);
#else
    ::munmap(ptr, bytes);
#endif
}

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

    static size_t bytes() noexcept {
        return s_allocatedBytes.load(std::memory_order_relaxed);
    }
};

} // namespace lf::bench

void* operator new(std::size_t size) {
    if (lf::bench::AllocationTracker::s_active.load(std::memory_order_relaxed)) {
        lf::bench::AllocationTracker::s_allocationCount.fetch_add(1, std::memory_order_relaxed);
        lf::bench::AllocationTracker::s_allocatedBytes.fetch_add(size, std::memory_order_relaxed);
    }
    void* p = std::malloc(size);
    if (p == nullptr) {
        std::abort();
    }
    return p;
}

void* operator new[](std::size_t size) {
    if (lf::bench::AllocationTracker::s_active.load(std::memory_order_relaxed)) {
        lf::bench::AllocationTracker::s_allocationCount.fetch_add(1, std::memory_order_relaxed);
        lf::bench::AllocationTracker::s_allocatedBytes.fetch_add(size, std::memory_order_relaxed);
    }
    void* p = std::malloc(size);
    if (p == nullptr) {
        std::abort();
    }
    return p;
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    if (lf::bench::AllocationTracker::s_active.load(std::memory_order_relaxed)) {
        lf::bench::AllocationTracker::s_allocationCount.fetch_add(1, std::memory_order_relaxed);
        lf::bench::AllocationTracker::s_allocatedBytes.fetch_add(size, std::memory_order_relaxed);
    }
    return std::malloc(size);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    if (lf::bench::AllocationTracker::s_active.load(std::memory_order_relaxed)) {
        lf::bench::AllocationTracker::s_allocationCount.fetch_add(1, std::memory_order_relaxed);
        lf::bench::AllocationTracker::s_allocatedBytes.fetch_add(size, std::memory_order_relaxed);
    }
    return std::malloc(size);
}

void operator delete(void* p) noexcept {
    if (lf::bench::AllocationTracker::s_active.load(std::memory_order_relaxed)) {
        lf::bench::AllocationTracker::s_deallocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}

void operator delete[](void* p) noexcept {
    if (lf::bench::AllocationTracker::s_active.load(std::memory_order_relaxed)) {
        lf::bench::AllocationTracker::s_deallocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}

void operator delete(void* p, std::size_t) noexcept {
    if (lf::bench::AllocationTracker::s_active.load(std::memory_order_relaxed)) {
        lf::bench::AllocationTracker::s_deallocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}

void operator delete[](void* p, std::size_t) noexcept {
    if (lf::bench::AllocationTracker::s_active.load(std::memory_order_relaxed)) {
        lf::bench::AllocationTracker::s_deallocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}

void operator delete(void* p, const std::nothrow_t&) noexcept {
    if (lf::bench::AllocationTracker::s_active.load(std::memory_order_relaxed)) {
        lf::bench::AllocationTracker::s_deallocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}

void operator delete[](void* p, const std::nothrow_t&) noexcept {
    if (lf::bench::AllocationTracker::s_active.load(std::memory_order_relaxed)) {
        lf::bench::AllocationTracker::s_deallocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}

void operator delete(void* p, std::size_t, const std::nothrow_t&) noexcept {
    if (lf::bench::AllocationTracker::s_active.load(std::memory_order_relaxed)) {
        lf::bench::AllocationTracker::s_deallocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}

void operator delete[](void* p, std::size_t, const std::nothrow_t&) noexcept {
    if (lf::bench::AllocationTracker::s_active.load(std::memory_order_relaxed)) {
        lf::bench::AllocationTracker::s_deallocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}

// =============================================================================
// Realistic Rendering Compute Workload: Frustum vs Bounding Sphere Culling
// =============================================================================

namespace lf::bench {

struct FrustumPlane {
    float x{0.0f}, y{0.0f}, z{0.0f}, d{0.0f};
};

struct alignas(16) ClusterBoundingSphere {
    float x{0.0f}, y{0.0f}, z{0.0f}, r{0.0f};
};

inline const FrustumPlane DEFAULT_FRUSTUM[6] = {
    { 1.0f,  0.0f,  0.0f, 100.0f}, // Left
    {-1.0f,  0.0f,  0.0f, 100.0f}, // Right
    { 0.0f,  1.0f,  0.0f, 100.0f}, // Bottom
    { 0.0f, -1.0f,  0.0f, 100.0f}, // Top
    { 0.0f,  0.0f,  1.0f,   1.0f}, // Near
    { 0.0f,  0.0f, -1.0f, 500.0f}  // Far
};

LF_NODISCARD inline bool cullCluster(const ClusterBoundingSphere& sphere, const FrustumPlane planes[6]) noexcept {
    for (int p = 0; p < 6; ++p) {
        float dist = planes[p].x * sphere.x + planes[p].y * sphere.y + planes[p].z * sphere.z + planes[p].d;
        if (dist < -sphere.r) {
            return false;
        }
    }
    return true;
}

LF_NODISCARD inline bool runClusterCull(size_t index, const FrustumPlane planes[6]) noexcept {
    float fi = static_cast<float>(index & 0xFFFF);
    ClusterBoundingSphere sphere{
        (fi * 0.017f) - 50.0f,
        (fi * 0.031f) - 50.0f,
        (fi * 0.043f) - 100.0f,
        15.0f
    };
    return cullCluster(sphere, planes);
}

// =============================================================================
// Classic Concurrency Baseline
// =============================================================================

class ClassicThreadPool {
public:
    explicit ClassicThreadPool(size_t threadCount) {
        workers_.reserve(threadCount);
        for (size_t i = 0; i < threadCount; ++i) {
            workers_.emplace_back([this, i]() {
                workerLoop(i);
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

    void enqueue(std::function<void()> task) {
        {
            LF_ZONE_SCOPED_N("Classic::Enqueue");
            std::unique_lock<std::mutex> lock(queueMutex_);
            tasks_.push(std::move(task));
        }
        cv_.notify_one();
    }

    size_t workerCount() const noexcept {
        return workers_.size();
    }

private:
    void workerLoop(size_t workerIdx) {
        std::string threadName = "Classic-Worker-" + std::to_string(workerIdx);
        LF_SET_THREAD_NAME(threadName.c_str());
        while (true) {
            std::function<void()> task;
            {
                LF_ZONE_SCOPED_N("Classic::QueueWait");
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
            {
                LF_ZONE_SCOPED_N("Classic::TaskRun");
                task();
            }
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

    void clear() {
        nodes_.clear();
    }

    size_t nodeCount() const noexcept {
        return nodes_.size();
    }

    void executeAndWait(ClassicThreadPool& pool) {
        LF_ZONE_SCOPED_N("Classic::executeAndWait");
        if (nodes_.empty()) return;
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

        for (auto& node : nodes_) {
            if (node->initialInDegree == 0) {
                Node* raw = node.get();
                pool.enqueue([&executeNode, &ctx, raw]() {
                    executeNode(executeNode, &ctx, raw);
                });
            }
        }

        std::unique_lock<std::mutex> lock(ctx.completionMutex);
        ctx.completionCv.wait(lock, [&ctx]() {
            return ctx.completed;
        });
    }

private:
    std::vector<std::unique_ptr<Node>> nodes_;
};

inline void classicParallelFor(
    ClassicThreadPool& pool,
    size_t count,
    size_t batchSize,
    const std::function<void(size_t, size_t)>& chunkFn
) {
    if (count == 0) return;
    batchSize = std::max<size_t>(1, batchSize);
    size_t numChunks = (count + batchSize - 1) / batchSize;

    std::atomic<size_t> remainingChunks{numChunks};
    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;

    for (size_t chunkIdx = 0; chunkIdx < numChunks; ++chunkIdx) {
        size_t start = chunkIdx * batchSize;
        size_t end = std::min(start + batchSize, count);
        pool.enqueue([start, end, &chunkFn, &remainingChunks, &mtx, &cv, &done]() {
            chunkFn(start, end);
            if (remainingChunks.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                std::unique_lock<std::mutex> lock(mtx);
                done = true;
                cv.notify_one();
            }
        });
    }

    std::unique_lock<std::mutex> lock(mtx);
    cv.wait(lock, [&done]() { return done; });
}

// =============================================================================
// Latency Statistics & Sweep Data Models
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
    double throughputMTasksSec{0.0};
    size_t steadyStateAllocs{0};
    size_t steadyStateBytes{0};

    static LatencyStats compute(
        std::vector<double>& samples,
        size_t taskCount,
        size_t allocs = 0,
        size_t bytes = 0
    ) {
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

        double throughput = 0.0;
        if (mean > 0.0) {
            throughput = (static_cast<double>(taskCount) / (mean / 1'000'000.0)) / 1'000'000.0; // M tasks/sec
        }

        return LatencyStats{
            .minUs = min,
            .maxUs = max,
            .meanUs = mean,
            .p50Us = percentile(0.50),
            .p95Us = percentile(0.95),
            .p99Us = percentile(0.99),
            .throughputMTasksSec = throughput,
            .steadyStateAllocs = allocs,
            .steadyStateBytes = bytes
        };
    }
};

struct SweepResult {
    std::string topology;
    std::string workload;
    size_t taskCount{0};
    size_t threadCount{0};
    size_t iterations{0};
    LatencyStats lightflow;
    LatencyStats classic;
    double speedupMean{1.0};
    double speedupP50{1.0};
};

// =============================================================================
// Benchmark Runner Functions
// =============================================================================

SweepResult benchmarkParallelFor(
    size_t taskCount,
    const std::string& workload,
    size_t iterations,
    size_t threadCount
) {
    bool isCull = (workload == "cluster_cull");
    size_t batchSize = std::max<size_t>(64, std::min<size_t>(4096, taskCount / (threadCount * 8)));
    if (batchSize == 0) batchSize = 64;

    // 1. LightFlow Setup
    lf::SchedulerConfig lfConfig{
        .workerCount = static_cast<lf::u32>(threadCount),
        .initialDequeCapacity = 65536
    };
    lf::TaskScheduler lfScheduler(lfConfig);

    std::atomic<size_t> lfResultSink{0};
    auto lfChunkFn = [&](size_t start, size_t end) noexcept {
        size_t localVisible = 0;
        if (isCull) {
            for (size_t i = start; i < end; ++i) {
                if (runClusterCull(i, DEFAULT_FRUSTUM)) {
                    ++localVisible;
                }
            }
        } else {
            localVisible = end - start;
        }
        lfResultSink.fetch_add(localVisible, std::memory_order_relaxed);
    };

    lf::TaskGraph lfGraph;
    lfGraph.parallelFor("PFChunk", taskCount, batchSize, lfChunkFn);

    // Warm-up
    for (int w = 0; w < 3; ++w) {
        lfScheduler.runAndWait(lfGraph);
    }

    // Steady-state allocation tracking
    AllocationTracker::reset();
    AllocationTracker::enable();
    lfScheduler.runAndWait(lfGraph);
    AllocationTracker::disable();
    size_t lfAllocs = AllocationTracker::allocations();
    size_t lfBytes = AllocationTracker::bytes();

    // Measurement iterations
    std::vector<double> lfSamples;
    lfSamples.reserve(iterations);
    for (size_t it = 0; it < iterations; ++it) {
        lfResultSink.store(0, std::memory_order_relaxed);
        LF_FRAME_MARK_NAMED("LightFlow_ParallelFor");
        auto t0 = Clock::now();
        {
            LF_ZONE_SCOPED_N("LightFlow::parallelFor");
            lfScheduler.runAndWait(lfGraph);
        }
        auto t1 = Clock::now();
        lfSamples.push_back(std::chrono::duration_cast<DurationUs>(t1 - t0).count());
    }

    // 2. Classic Baseline Setup
    ClassicThreadPool classicPool(threadCount);
    std::atomic<size_t> classicResultSink{0};
    auto classicChunkFn = [&](size_t start, size_t end) {
        size_t localVisible = 0;
        if (isCull) {
            for (size_t i = start; i < end; ++i) {
                if (runClusterCull(i, DEFAULT_FRUSTUM)) {
                    ++localVisible;
                }
            }
        } else {
            localVisible = end - start;
        }
        classicResultSink.fetch_add(localVisible, std::memory_order_relaxed);
    };

    // Warm-up
    classicParallelFor(classicPool, taskCount, batchSize, classicChunkFn);

    // Steady-state allocation tracking
    AllocationTracker::reset();
    AllocationTracker::enable();
    classicParallelFor(classicPool, taskCount, batchSize, classicChunkFn);
    AllocationTracker::disable();
    size_t classicAllocs = AllocationTracker::allocations();
    size_t classicBytes = AllocationTracker::bytes();

    // Measurement iterations
    std::vector<double> classicSamples;
    classicSamples.reserve(iterations);
    for (size_t it = 0; it < iterations; ++it) {
        classicResultSink.store(0, std::memory_order_relaxed);
        LF_FRAME_MARK_NAMED("Classic_ParallelFor");
        auto t0 = Clock::now();
        {
            LF_ZONE_SCOPED_N("Classic::parallelFor");
            classicParallelFor(classicPool, taskCount, batchSize, classicChunkFn);
        }
        auto t1 = Clock::now();
        classicSamples.push_back(std::chrono::duration_cast<DurationUs>(t1 - t0).count());
    }

    auto lfStats = LatencyStats::compute(lfSamples, taskCount, lfAllocs, lfBytes);
    auto classicStats = LatencyStats::compute(classicSamples, taskCount, classicAllocs, classicBytes);
    double speedupMean = (lfStats.meanUs > 0.0) ? (classicStats.meanUs / lfStats.meanUs) : 1.0;
    double speedupP50 = (lfStats.p50Us > 0.0) ? (classicStats.p50Us / lfStats.p50Us) : 1.0;

    return SweepResult{
        .topology = "parallel_for",
        .workload = workload,
        .taskCount = taskCount,
        .threadCount = threadCount,
        .iterations = iterations,
        .lightflow = lfStats,
        .classic = classicStats,
        .speedupMean = speedupMean,
        .speedupP50 = speedupP50
    };
}

SweepResult benchmarkWavefront(
    size_t taskCount,
    const std::string& workload,
    size_t iterations,
    size_t threadCount
) {
    bool isCull = (workload == "cluster_cull");
    size_t numBatches = (taskCount >= 100000) ? std::max<size_t>(10, taskCount / 10000) : 1;
    size_t batchDiv = (numBatches > 1) ? (taskCount / numBatches) : 1;

    // 1. LightFlow Setup
    lf::SchedulerConfig lfConfig{
        .workerCount = static_cast<lf::u32>(threadCount),
        .initialDequeCapacity = (taskCount >= 5000000) ? 2097152ULL : ((taskCount >= 500000) ? 1048576ULL : 131072ULL)
    };
    lf::TaskScheduler lfScheduler(lfConfig);

    lf::TaskGraph lfGraph;
    std::atomic<size_t> lfResultSink{0};

    lf::TaskHandle lfRoot = lfGraph.emplace([]() noexcept {});
    lf::TaskHandle lfJoin = lfGraph.emplace([]() noexcept {});

    if (numBatches > 1) {
        std::vector<lf::TaskHandle> collectors;
        collectors.reserve(numBatches);
        for (size_t b = 0; b < numBatches; ++b) {
            auto coll = lfGraph.emplace([]() noexcept {});
            coll.precede(lfJoin);
            collectors.push_back(coll);
        }
        for (size_t i = 0; i < taskCount; ++i) {
            auto leaf = lfGraph.emplace([i, isCull, &lfResultSink]() noexcept {
                if (isCull) {
                    if (runClusterCull(i, DEFAULT_FRUSTUM)) {
                        lfResultSink.fetch_add(1, std::memory_order_relaxed);
                    }
                } else {
                    lfResultSink.fetch_add(1, std::memory_order_relaxed);
                }
            });
            lfRoot.precede(leaf);
            leaf.precede(collectors[std::min(i / batchDiv, numBatches - 1)]);
        }
    } else {
        for (size_t i = 0; i < taskCount; ++i) {
            auto leaf = lfGraph.emplace([i, isCull, &lfResultSink]() noexcept {
                if (isCull) {
                    if (runClusterCull(i, DEFAULT_FRUSTUM)) {
                        lfResultSink.fetch_add(1, std::memory_order_relaxed);
                    }
                } else {
                    lfResultSink.fetch_add(1, std::memory_order_relaxed);
                }
            });
            lfRoot.precede(leaf);
            leaf.precede(lfJoin);
        }
    }

    // Warm-up
    for (int w = 0; w < 3; ++w) {
        lfScheduler.runAndWait(lfGraph);
    }

    // Steady-state allocation tracking
    AllocationTracker::reset();
    AllocationTracker::enable();
    lfScheduler.runAndWait(lfGraph);
    AllocationTracker::disable();
    size_t lfAllocs = AllocationTracker::allocations();
    size_t lfBytes = AllocationTracker::bytes();

    // Measurement iterations
    std::vector<double> lfSamples;
    lfSamples.reserve(iterations);
    for (size_t it = 0; it < iterations; ++it) {
        lfResultSink.store(0, std::memory_order_relaxed);
        LF_FRAME_MARK_NAMED("LightFlow_Wavefront");
        auto t0 = Clock::now();
        {
            LF_ZONE_SCOPED_N("LightFlow::Wavefront");
            lfScheduler.runAndWait(lfGraph);
        }
        auto t1 = Clock::now();
        lfSamples.push_back(std::chrono::duration_cast<DurationUs>(t1 - t0).count());
    }

    // 2. Classic Baseline Setup
    ClassicThreadPool classicPool(threadCount);
    ClassicTaskGraph classicGraph;
    std::atomic<size_t> classicResultSink{0};

    auto* classicRoot = classicGraph.emplace([]() {});
    auto* classicJoin = classicGraph.emplace([]() {});

    if (numBatches > 1) {
        std::vector<ClassicTaskGraph::Node*> collectors;
        collectors.reserve(numBatches);
        for (size_t b = 0; b < numBatches; ++b) {
            auto* coll = classicGraph.emplace([]() {});
            classicGraph.addEdge(coll, classicJoin);
            collectors.push_back(coll);
        }
        for (size_t i = 0; i < taskCount; ++i) {
            auto* leaf = classicGraph.emplace([i, isCull, &classicResultSink]() {
                if (isCull) {
                    if (runClusterCull(i, DEFAULT_FRUSTUM)) {
                        classicResultSink.fetch_add(1, std::memory_order_relaxed);
                    }
                } else {
                    classicResultSink.fetch_add(1, std::memory_order_relaxed);
                }
            });
            classicGraph.addEdge(classicRoot, leaf);
            classicGraph.addEdge(leaf, collectors[std::min(i / batchDiv, numBatches - 1)]);
        }
    } else {
        for (size_t i = 0; i < taskCount; ++i) {
            auto* leaf = classicGraph.emplace([i, isCull, &classicResultSink]() {
                if (isCull) {
                    if (runClusterCull(i, DEFAULT_FRUSTUM)) {
                        classicResultSink.fetch_add(1, std::memory_order_relaxed);
                    }
                } else {
                    classicResultSink.fetch_add(1, std::memory_order_relaxed);
                }
            });
            classicGraph.addEdge(classicRoot, leaf);
            classicGraph.addEdge(leaf, classicJoin);
        }
    }

    // Warm-up
    classicGraph.reset();
    classicGraph.executeAndWait(classicPool);

    // Steady-state allocation tracking
    AllocationTracker::reset();
    AllocationTracker::enable();
    classicGraph.reset();
    classicGraph.executeAndWait(classicPool);
    AllocationTracker::disable();
    size_t classicAllocs = AllocationTracker::allocations();
    size_t classicBytes = AllocationTracker::bytes();

    // Measurement iterations
    std::vector<double> classicSamples;
    classicSamples.reserve(iterations);
    for (size_t it = 0; it < iterations; ++it) {
        classicResultSink.store(0, std::memory_order_relaxed);
        classicGraph.reset();
        LF_FRAME_MARK_NAMED("Classic_Wavefront");
        auto t0 = Clock::now();
        {
            LF_ZONE_SCOPED_N("Classic::Wavefront");
            classicGraph.executeAndWait(classicPool);
        }
        auto t1 = Clock::now();
        classicSamples.push_back(std::chrono::duration_cast<DurationUs>(t1 - t0).count());
    }

    auto lfStats = LatencyStats::compute(lfSamples, taskCount, lfAllocs, lfBytes);
    auto classicStats = LatencyStats::compute(classicSamples, taskCount, classicAllocs, classicBytes);
    double speedupMean = (lfStats.meanUs > 0.0) ? (classicStats.meanUs / lfStats.meanUs) : 1.0;
    double speedupP50 = (lfStats.p50Us > 0.0) ? (classicStats.p50Us / lfStats.p50Us) : 1.0;

    return SweepResult{
        .topology = "wavefront",
        .workload = workload,
        .taskCount = taskCount,
        .threadCount = threadCount,
        .iterations = iterations,
        .lightflow = lfStats,
        .classic = classicStats,
        .speedupMean = speedupMean,
        .speedupP50 = speedupP50
    };
}

SweepResult benchmarkPipeline(
    size_t taskCount,
    const std::string& workload,
    size_t iterations,
    size_t threadCount
) {
    bool isCull = (workload == "cluster_cull");
    constexpr size_t NUM_STAGES = 10;
    size_t tasksPerStage = std::max<size_t>(1, taskCount / NUM_STAGES);

    // 1. LightFlow Setup
    lf::SchedulerConfig lfConfig{
        .workerCount = static_cast<lf::u32>(threadCount),
        .initialDequeCapacity = (taskCount >= 500000) ? 262144ULL : 131072ULL
    };
    lf::TaskScheduler lfScheduler(lfConfig);

    lf::TaskGraph lfGraph;
    std::atomic<size_t> lfResultSink{0};

    std::vector<lf::TaskHandle> stageBarriers;
    stageBarriers.reserve(NUM_STAGES + 1);
    for (size_t s = 0; s <= NUM_STAGES; ++s) {
        stageBarriers.push_back(lfGraph.emplace([]() noexcept {}));
    }

    size_t numBatches = (tasksPerStage >= 30000) ? std::max<size_t>(4, tasksPerStage / 10000) : 1;
    size_t batchDiv = (numBatches > 1) ? (tasksPerStage / numBatches) : 1;

    size_t currentIdx = 0;
    for (size_t s = 0; s < NUM_STAGES; ++s) {
        std::vector<lf::TaskHandle> stageCollectors;
        if (numBatches > 1) {
            stageCollectors.reserve(numBatches);
            for (size_t b = 0; b < numBatches; ++b) {
                auto coll = lfGraph.emplace([]() noexcept {});
                coll.precede(stageBarriers[s + 1]);
                stageCollectors.push_back(coll);
            }
        }

        for (size_t i = 0; i < tasksPerStage; ++i, ++currentIdx) {
            auto task = lfGraph.emplace([currentIdx, isCull, &lfResultSink]() noexcept {
                if (isCull) {
                    if (runClusterCull(currentIdx, DEFAULT_FRUSTUM)) {
                        lfResultSink.fetch_add(1, std::memory_order_relaxed);
                    }
                } else {
                    lfResultSink.fetch_add(1, std::memory_order_relaxed);
                }
            });
            stageBarriers[s].precede(task);
            if (numBatches > 1) {
                task.precede(stageCollectors[std::min(i / batchDiv, numBatches - 1)]);
            } else {
                task.precede(stageBarriers[s + 1]);
            }
        }
    }

    // Warm-up
    for (int w = 0; w < 3; ++w) {
        lfScheduler.runAndWait(lfGraph);
    }

    // Steady-state allocation tracking
    AllocationTracker::reset();
    AllocationTracker::enable();
    lfScheduler.runAndWait(lfGraph);
    AllocationTracker::disable();
    size_t lfAllocs = AllocationTracker::allocations();
    size_t lfBytes = AllocationTracker::bytes();

    // Measurement iterations
    std::vector<double> lfSamples;
    lfSamples.reserve(iterations);
    for (size_t it = 0; it < iterations; ++it) {
        lfResultSink.store(0, std::memory_order_relaxed);
        LF_FRAME_MARK_NAMED("LightFlow_Pipeline");
        auto t0 = Clock::now();
        {
            LF_ZONE_SCOPED_N("LightFlow::Pipeline");
            lfScheduler.runAndWait(lfGraph);
        }
        auto t1 = Clock::now();
        lfSamples.push_back(std::chrono::duration_cast<DurationUs>(t1 - t0).count());
    }

    // 2. Classic Baseline Setup
    ClassicThreadPool classicPool(threadCount);
    ClassicTaskGraph classicGraph;
    std::atomic<size_t> classicResultSink{0};

    std::vector<ClassicTaskGraph::Node*> classicBarriers;
    classicBarriers.reserve(NUM_STAGES + 1);
    for (size_t s = 0; s <= NUM_STAGES; ++s) {
        classicBarriers.push_back(classicGraph.emplace([]() {}));
    }

    currentIdx = 0;
    for (size_t s = 0; s < NUM_STAGES; ++s) {
        std::vector<ClassicTaskGraph::Node*> classicCollectors;
        if (numBatches > 1) {
            classicCollectors.reserve(numBatches);
            for (size_t b = 0; b < numBatches; ++b) {
                auto* coll = classicGraph.emplace([]() {});
                classicGraph.addEdge(coll, classicBarriers[s + 1]);
                classicCollectors.push_back(coll);
            }
        }

        for (size_t i = 0; i < tasksPerStage; ++i, ++currentIdx) {
            auto* task = classicGraph.emplace([currentIdx, isCull, &classicResultSink]() {
                if (isCull) {
                    if (runClusterCull(currentIdx, DEFAULT_FRUSTUM)) {
                        classicResultSink.fetch_add(1, std::memory_order_relaxed);
                    }
                } else {
                    classicResultSink.fetch_add(1, std::memory_order_relaxed);
                }
            });
            classicGraph.addEdge(classicBarriers[s], task);
            if (numBatches > 1) {
                classicGraph.addEdge(task, classicCollectors[std::min(i / batchDiv, numBatches - 1)]);
            } else {
                classicGraph.addEdge(task, classicBarriers[s + 1]);
            }
        }
    }

    // Warm-up
    classicGraph.reset();
    classicGraph.executeAndWait(classicPool);

    // Steady-state allocation tracking
    AllocationTracker::reset();
    AllocationTracker::enable();
    classicGraph.reset();
    classicGraph.executeAndWait(classicPool);
    AllocationTracker::disable();
    size_t classicAllocs = AllocationTracker::allocations();
    size_t classicBytes = AllocationTracker::bytes();

    // Measurement iterations
    std::vector<double> classicSamples;
    classicSamples.reserve(iterations);
    for (size_t it = 0; it < iterations; ++it) {
        classicResultSink.store(0, std::memory_order_relaxed);
        classicGraph.reset();
        LF_FRAME_MARK_NAMED("Classic_Pipeline");
        auto t0 = Clock::now();
        {
            LF_ZONE_SCOPED_N("Classic::Pipeline");
            classicGraph.executeAndWait(classicPool);
        }
        auto t1 = Clock::now();
        classicSamples.push_back(std::chrono::duration_cast<DurationUs>(t1 - t0).count());
    }

    auto lfStats = LatencyStats::compute(lfSamples, taskCount, lfAllocs, lfBytes);
    auto classicStats = LatencyStats::compute(classicSamples, taskCount, classicAllocs, classicBytes);
    double speedupMean = (lfStats.meanUs > 0.0) ? (classicStats.meanUs / lfStats.meanUs) : 1.0;
    double speedupP50 = (lfStats.p50Us > 0.0) ? (classicStats.p50Us / lfStats.p50Us) : 1.0;

    return SweepResult{
        .topology = "pipeline",
        .workload = workload,
        .taskCount = taskCount,
        .threadCount = threadCount,
        .iterations = iterations,
        .lightflow = lfStats,
        .classic = classicStats,
        .speedupMean = speedupMean,
        .speedupP50 = speedupP50
    };
}

// =============================================================================
// Formatted Console Output
// =============================================================================

void printResultTable(const SweepResult& res) {
    std::cout << "\n+---------------------------------------------------------------------------------------------------------+\n"
              << "| " << std::left << std::setw(103)
              << ("Topology: " + res.topology + " | Workload: " + res.workload + " | Tasks: " + std::to_string(res.taskCount))
              << " |\n"
              << "+---------------------------------------------------------------------------------------------------------+\n"
              << "| " << std::left << std::setw(24) << "Framework"
              << std::right
              << std::setw(12) << "Mean (us)"
              << std::setw(12) << "P50 (us)"
              << std::setw(12) << "P95 (us)"
              << std::setw(14) << "M Tasks/sec"
              << std::setw(14) << "Heap Allocs"
              << std::setw(15) << "Speedup"
              << " |\n"
              << "+---------------------------------------------------------------------------------------------------------+\n";

    char lfSpeedup[32];
    std::snprintf(lfSpeedup, sizeof(lfSpeedup), "%.2fx", res.speedupMean);

    auto printRow = [](const char* name, const LatencyStats& s, const char* speedupStr) {
        std::cout << "| " << std::left << std::setw(24) << name
                  << std::right << std::fixed << std::setprecision(1)
                  << std::setw(12) << s.meanUs
                  << std::setw(12) << s.p50Us
                  << std::setw(12) << s.p95Us
                  << std::setprecision(2)
                  << std::setw(14) << s.throughputMTasksSec
                  << std::setw(14) << s.steadyStateAllocs
                  << std::setw(15) << speedupStr
                  << " |\n";
    };

    printRow("LightFlow (Lock-Free)", res.lightflow, lfSpeedup);
    printRow("Classic ThreadPool", res.classic, "1.00x (ref)");

    std::cout << "+---------------------------------------------------------------------------------------------------------+\n"
              << "| >>> LightFlow is " << std::fixed << std::setprecision(2) << res.speedupMean
              << "x faster on average (P50: " << res.speedupP50 << "x) | Steady-State Mallocs: "
              << res.lightflow.steadyStateAllocs << " vs " << res.classic.steadyStateAllocs << " <<<\n"
              << "+---------------------------------------------------------------------------------------------------------+\n";
}

// =============================================================================
// JSON Serialization
// =============================================================================

void writeJsonResults(const std::string& outputPath, const std::vector<SweepResult>& results, size_t threadCount) {
    std::ofstream out(outputPath);
    if (!out.is_open()) {
        std::cerr << "Failed to open output file for JSON: " << outputPath << "\n";
        return;
    }

    out << "{\n"
        << "  \"meta\": {\n"
        << "    \"engine\": \"LightFlow Task Graph Engine\",\n"
        << "    \"baseline\": \"Classic ThreadPool (std::mutex + std::condition_variable + std::queue<std::function<void()>>)\",\n"
        << "    \"thread_count\": " << threadCount << ",\n"
        << "    \"timestamp\": " << std::chrono::duration_cast<std::chrono::seconds>(Clock::now().time_since_epoch()).count() << "\n"
        << "  },\n"
        << "  \"results\": [\n";

    for (size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        out << "    {\n"
            << "      \"topology\": \"" << r.topology << "\",\n"
            << "      \"workload\": \"" << r.workload << "\",\n"
            << "      \"task_count\": " << r.taskCount << ",\n"
            << "      \"thread_count\": " << r.threadCount << ",\n"
            << "      \"iterations\": " << r.iterations << ",\n"
            << "      \"speedup_mean\": " << std::fixed << std::setprecision(3) << r.speedupMean << ",\n"
            << "      \"speedup_p50\": " << std::fixed << std::setprecision(3) << r.speedupP50 << ",\n"
            << "      \"lightflow\": {\n"
            << "        \"min_us\": " << r.lightflow.minUs << ",\n"
            << "        \"mean_us\": " << r.lightflow.meanUs << ",\n"
            << "        \"p50_us\": " << r.lightflow.p50Us << ",\n"
            << "        \"p95_us\": " << r.lightflow.p95Us << ",\n"
            << "        \"p99_us\": " << r.lightflow.p99Us << ",\n"
            << "        \"max_us\": " << r.lightflow.maxUs << ",\n"
            << "        \"throughput_m_tasks_sec\": " << r.lightflow.throughputMTasksSec << ",\n"
            << "        \"steady_state_allocs\": " << r.lightflow.steadyStateAllocs << ",\n"
            << "        \"steady_state_bytes\": " << r.lightflow.steadyStateBytes << "\n"
            << "      },\n"
            << "      \"classic\": {\n"
            << "        \"min_us\": " << r.classic.minUs << ",\n"
            << "        \"mean_us\": " << r.classic.meanUs << ",\n"
            << "        \"p50_us\": " << r.classic.p50Us << ",\n"
            << "        \"p95_us\": " << r.classic.p95Us << ",\n"
            << "        \"p99_us\": " << r.classic.p99Us << ",\n"
            << "        \"max_us\": " << r.classic.maxUs << ",\n"
            << "        \"throughput_m_tasks_sec\": " << r.classic.throughputMTasksSec << ",\n"
            << "        \"steady_state_allocs\": " << r.classic.steadyStateAllocs << ",\n"
            << "        \"steady_state_bytes\": " << r.classic.steadyStateBytes << "\n"
            << "      }\n"
            << "    }" << (i + 1 < results.size() ? ",\n" : "\n");
    }

    out << "  ]\n"
        << "}\n";

    std::cout << "\n>>> Benchmark results successfully saved to: " << outputPath << " <<<\n";
}

} // namespace lf::bench

// =============================================================================
// CLI Entry Point
// =============================================================================

int main(int argc, char* argv[]) {
    using namespace lf::bench;

    lf::MemoryCallbacks benchCallbacks{
        .alloc = &bench_alloc,
        .free = &bench_free,
        .user_data = nullptr
    };
    lf::BlockPool::set_global_callbacks(benchCallbacks);

    std::string outputPath = "docs/benchmarks/results.json";
    bool quick = false;
    size_t maxTasks = 10'000'000;
    std::string filterTopology = "all";
    std::string filterMode = "both";

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--output" && i + 1 < argc) {
            outputPath = argv[++i];
        } else if (arg == "--quick") {
            quick = true;
        } else if (arg == "--max-tasks" && i + 1 < argc) {
            maxTasks = static_cast<size_t>(std::stoull(argv[++i]));
        } else if (arg == "--topology" && i + 1 < argc) {
            filterTopology = argv[++i];
        } else if (arg == "--mode" && i + 1 < argc) {
            filterMode = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [options]\n"
                      << "Options:\n"
                      << "  --output <file>     Path to output JSON results (default: docs/benchmarks/results.json)\n"
                      << "  --quick             Run fewer iterations for quick testing\n"
                      << "  --max-tasks <N>     Maximum tasks to sweep up to (default: 10000000)\n"
                      << "  --topology <type>   Filter topology: 'parallel_for', 'wavefront', 'pipeline', 'all'\n"
                      << "  --mode <type>       Filter mode: 'cluster_cull', 'orchestration', 'both'\n"
                      << "  --help              Display this help message\n";
            return 0;
        }
    }

    const size_t threadCount = std::max<size_t>(2, std::thread::hardware_concurrency());

    std::cout << "===========================================================================================================\n"
              << " LightFlow Concurrency Benchmark Sweep Engine (100 to " << maxTasks << " Tasks)\n"
              << " Worker Threads: " << threadCount << " | Output: " << outputPath << "\n"
              << "===========================================================================================================\n";

    std::vector<size_t> allTaskCounts = {100, 1000, 10000, 50000, 100000, 500000, 1000000, 5000000, 10000000};
    std::vector<size_t> taskCounts;
    for (size_t t : allTaskCounts) {
        if (t <= maxTasks) {
            taskCounts.push_back(t);
        }
    }

    auto getIterations = [quick](size_t tasks) -> size_t {
        if (quick) {
            if (tasks <= 1000) return 5;
            if (tasks <= 100000) return 3;
            if (tasks <= 1000000) return 2;
            return 1;
        }
        if (tasks <= 1000) return 20;
        if (tasks <= 50000) return 10;
        if (tasks <= 100000) return 8;
        if (tasks <= 1000000) return 5;
        if (tasks <= 5000000) return 3;
        return 2;
    };

    std::vector<std::string> workloads;
    if (filterMode == "both" || filterMode == "cluster_cull") {
        workloads.push_back("cluster_cull");
    }
    if (filterMode == "both" || filterMode == "orchestration") {
        workloads.push_back("orchestration");
    }

    std::vector<SweepResult> results;

    // -------------------------------------------------------------------------
    // 1. Parallel For Sweep
    // -------------------------------------------------------------------------
    if (filterTopology == "all" || filterTopology == "parallel_for") {
        std::cout << "\n>>> Executing Parallel For Sweeps <<<\n";
        for (const auto& wl : workloads) {
            for (size_t count : taskCounts) {
                size_t iters = getIterations(count);
                std::cout << "Running ParallelFor [" << wl << "] Tasks: " << count
                          << " (iters: " << iters << ")... " << std::flush;
                SweepResult res = benchmarkParallelFor(count, wl, iters, threadCount);
                printResultTable(res);
                results.push_back(res);
            }
        }
    }

    // -------------------------------------------------------------------------
    // 2. Wavefront (Fan-Out & Fan-In DAG) Sweep
    // -------------------------------------------------------------------------
    if (filterTopology == "all" || filterTopology == "wavefront") {
        std::cout << "\n>>> Executing Wavefront DAG Sweeps <<<\n";
        for (const auto& wl : workloads) {
            for (size_t count : taskCounts) {
                size_t iters = getIterations(count);
                std::cout << "Running Wavefront [" << wl << "] Tasks: " << count
                          << " (iters: " << iters << ")... " << std::flush;
                SweepResult res = benchmarkWavefront(count, wl, iters, threadCount);
                printResultTable(res);
                results.push_back(res);
            }
        }
    }

    // -------------------------------------------------------------------------
    // 3. Multi-Stage Pipeline Sweep
    // -------------------------------------------------------------------------
    if (filterTopology == "all" || filterTopology == "pipeline") {
        std::cout << "\n>>> Executing Multi-Stage Pipeline Sweeps <<<\n";
        for (const auto& wl : workloads) {
            for (size_t count : taskCounts) {
                // Pipeline DAG has multiple barrier sync points; limit to <= 1M tasks
                if (count > 1000000) continue;
                size_t iters = getIterations(count);
                std::cout << "Running Pipeline [" << wl << "] Tasks: " << count
                          << " (iters: " << iters << ")... " << std::flush;
                SweepResult res = benchmarkPipeline(count, wl, iters, threadCount);
                printResultTable(res);
                results.push_back(res);
            }
        }
    }

    // Output JSON
    writeJsonResults(outputPath, results, threadCount);

    return 0;
}
