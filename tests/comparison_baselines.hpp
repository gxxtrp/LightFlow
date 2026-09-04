#pragma once

// =============================================================================
// LightFlow Architectural Comparison Baselines
// Unified header providing comparative concurrency paradigms:
// 1. TaskflowRunner (Taskflow v3.8.0 Task Graph Engine)
// 2. FiberJobSystem (Stackful Fiber System - Naughty Dog GDC 2015 Model)
// 3. CoroutineTaskSystem (Modern C++20 Stackless Coroutine Dispatcher)
// 4. ClassicThreadPool & ClassicTaskGraph (Mutex + CV ThreadPool Baseline)
// =============================================================================

#if defined(__APPLE__)
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 600
#endif
#endif

#if defined(__APPLE__) || defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <queue>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <ucontext.h>
#endif

#if defined(LF_HAS_TASKFLOW) && LF_HAS_TASKFLOW
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wconversion"
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wshadow"
#pragma clang diagnostic ignored "-Wold-style-cast"
#pragma clang diagnostic ignored "-Wpedantic"
#pragma clang diagnostic ignored "-Wcast-qual"
#pragma clang diagnostic ignored "-Wundef"
#endif
#include <taskflow/taskflow.hpp>
#include <taskflow/algorithm/for_each.hpp>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#endif

namespace lf::comparison {

using Clock = std::chrono::high_resolution_clock;
using DurationUs = std::chrono::duration<double, std::micro>;

// =============================================================================
// Latency Statistics & Formatter
// =============================================================================

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

struct BenchmarkRow {
    std::string frameworkName;
    LatencyStats stats;
    size_t allocations{0};
    size_t allocatedBytes{0};
    double speedup{1.0};
    bool isReference{false};
};

inline void printMultiParadigmComparisonTable(
    const char* benchmarkTitle,
    size_t taskCount,
    size_t iterations,
    size_t threadCount,
    std::span<const BenchmarkRow> rows
) {
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

    for (const auto& row : rows) {
        char speedupBuf[32];
        if (row.isReference) {
            std::snprintf(speedupBuf, sizeof(speedupBuf), "1.00x (ref)");
        } else if (row.speedup >= 1.0) {
            std::snprintf(speedupBuf, sizeof(speedupBuf), "%.2fx", row.speedup);
        } else {
            std::snprintf(speedupBuf, sizeof(speedupBuf), "%.2fx slower", 1.0 / row.speedup);
        }

        std::cout << std::left << std::setw(30) << row.frameworkName
                  << std::right << std::fixed << std::setprecision(2)
                  << std::setw(14) << row.stats.minUs
                  << std::setw(14) << row.stats.meanUs
                  << std::setw(14) << row.stats.p50Us
                  << std::setw(14) << row.stats.p95Us
                  << std::setw(14) << row.stats.maxUs
                  << std::setw(18) << speedupBuf
                  << "\n";
    }

    std::cout << "========================================================================================================================\n\n";
}

inline void printComparisonTable(
    const char* benchmarkTitle,
    size_t taskCount,
    size_t iterations,
    size_t threadCount,
    const LatencyStats& lfStats,
    const LatencyStats& classicStats
) {
    double speedupMean = (lfStats.meanUs > 0.0) ? (classicStats.meanUs / lfStats.meanUs) : 1.0;
    double speedupP50 = (lfStats.p50Us > 0.0) ? (classicStats.p50Us / lfStats.p50Us) : 1.0;

    BenchmarkRow rows[2] = {
        {
            .frameworkName = "LightFlow (Lock-Free)",
            .stats = lfStats,
            .allocations = 0,
            .allocatedBytes = 0,
            .speedup = speedupMean,
            .isReference = false
        },
        {
            .frameworkName = "Classic ThreadPool",
            .stats = classicStats,
            .allocations = 0,
            .allocatedBytes = 0,
            .speedup = 1.0,
            .isReference = true
        }
    };

    printMultiParadigmComparisonTable(benchmarkTitle, taskCount, iterations, threadCount, rows);

    std::cout << " >>> LightFlow is " << std::fixed << std::setprecision(2) << speedupMean
              << "x faster on average (P50 speedup: " << speedupP50 << "x) <<<\n\n";
}

// =============================================================================
// 1. Classic Baseline: Mutex + CondVar ThreadPool & Dynamic Task Graph
// =============================================================================

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

// =============================================================================
// 2. Taskflow Comparative Harness (Taskflow v3.8.0)
// =============================================================================

#if defined(LF_HAS_TASKFLOW) && LF_HAS_TASKFLOW

class TaskflowRunner {
public:
    explicit TaskflowRunner(size_t threadCount)
        : executor_(threadCount) {}

    size_t workerCount() const noexcept {
        return executor_.num_workers();
    }

    tf::Executor& executor() noexcept {
        return executor_;
    }

    void runWavefront(
        size_t taskCount,
        size_t iterations,
        std::atomic<size_t>& counter,
        std::vector<double>& outSamples
    ) {
        tf::Taskflow tf;
        auto root = tf.emplace([&counter]() {
            counter.fetch_add(1, std::memory_order_relaxed);
        });

        auto join = tf.emplace([&counter]() {
            counter.fetch_add(1, std::memory_order_relaxed);
        });

        std::vector<tf::Task> leaves;
        leaves.reserve(taskCount - 2);
        for (size_t i = 0; i < taskCount - 2; ++i) {
            auto leaf = tf.emplace([&counter]() {
                counter.fetch_add(1, std::memory_order_relaxed);
            });
            root.precede(leaf);
            leaf.precede(join);
            leaves.push_back(leaf);
        }

        // Warm-up
        counter.store(0, std::memory_order_relaxed);
        executor_.run(tf).wait();

        outSamples.reserve(iterations);
        for (size_t it = 0; it < iterations; ++it) {
            counter.store(0, std::memory_order_relaxed);
            auto t0 = Clock::now();
            executor_.run(tf).wait();
            auto t1 = Clock::now();
            outSamples.push_back(std::chrono::duration_cast<DurationUs>(t1 - t0).count());
        }
    }

    void runPipeline(
        size_t stages,
        size_t tasksPerStage,
        size_t iterations,
        std::atomic<size_t>& counter,
        std::vector<double>& outSamples
    ) {
        tf::Taskflow tf;
        std::vector<std::vector<tf::Task>> stageTasks(stages);

        for (size_t s = 0; s < stages; ++s) {
            stageTasks[s].reserve(tasksPerStage);
            for (size_t t = 0; t < tasksPerStage; ++t) {
                stageTasks[s].push_back(tf.emplace([&counter]() {
                    counter.fetch_add(1, std::memory_order_relaxed);
                }));
            }
        }

        for (size_t s = 1; s < stages; ++s) {
            auto barrier = tf.emplace([]() {});
            for (auto& prev : stageTasks[s - 1]) {
                prev.precede(barrier);
            }
            for (auto& next : stageTasks[s]) {
                barrier.precede(next);
            }
        }

        // Warm-up
        counter.store(0, std::memory_order_relaxed);
        executor_.run(tf).wait();

        outSamples.reserve(iterations);
        for (size_t it = 0; it < iterations; ++it) {
            counter.store(0, std::memory_order_relaxed);
            auto t0 = Clock::now();
            executor_.run(tf).wait();
            auto t1 = Clock::now();
            outSamples.push_back(std::chrono::duration_cast<DurationUs>(t1 - t0).count());
        }
    }

    void runParallelFor(
        size_t itemCount,
        size_t batchSize,
        size_t iterations,
        std::vector<uint32_t>& data,
        std::vector<double>& outSamples
    ) {
        tf::Taskflow tf;
        tf.for_each_index(
            size_t(0),
            itemCount,
            size_t(1),
            [&data](size_t idx) {
                data[idx] = static_cast<uint32_t>(idx * 3 + 1);
            },
            tf::StaticPartitioner(batchSize)
        );

        // Warm-up
        executor_.run(tf).wait();

        outSamples.reserve(iterations);
        for (size_t it = 0; it < iterations; ++it) {
            auto t0 = Clock::now();
            executor_.run(tf).wait();
            auto t1 = Clock::now();
            outSamples.push_back(std::chrono::duration_cast<DurationUs>(t1 - t0).count());
        }
    }

private:
    tf::Executor executor_;
};

#endif // LF_HAS_TASKFLOW

// =============================================================================
// 3. Stackful Fiber Job System (Naughty Dog GDC 2015 Model)
// =============================================================================

class FiberJobSystem {
public:
    static constexpr size_t FIBER_STACK_SIZE = 64 * 1024; // 64 KB fixed stack
    static constexpr size_t DEFAULT_FIBER_POOL_SIZE = 256;

    struct Job {
        void (*func)(void*) noexcept{nullptr};
        void* arg{nullptr};
        std::atomic<uint32_t>* counter{nullptr};
    };

    enum class FiberState : uint8_t {
        Free,
        Running,
        Waiting
    };

    enum class YieldReason : uint8_t {
        Finished,
        WaitingOnCounter
    };

    struct Fiber {
        size_t id{0};
        FiberJobSystem* system{nullptr};
        alignas(64) std::vector<uint8_t> stack;
#if defined(_WIN32)
        LPVOID fiberHandle{nullptr};
        LPVOID callerFiber{nullptr};
#else
        ucontext_t context{};
        ucontext_t* callerContext{nullptr};
#endif
        FiberState state{FiberState::Free};
        YieldReason lastYieldReason{YieldReason::Finished};
        Job currentJob{};
        std::atomic<uint32_t>* waitingCounter{nullptr};
        uint32_t targetValue{0};

        void init(size_t fiberId, FiberJobSystem* sys) {
            id = fiberId;
            system = sys;
            stack.resize(FIBER_STACK_SIZE);
#if defined(_WIN32)
            fiberHandle = CreateFiber(FIBER_STACK_SIZE, fiberTrampoline, this);
#else
            getcontext(&context);
            context.uc_stack.ss_sp = stack.data();
            context.uc_stack.ss_size = stack.size();
            context.uc_link = nullptr;

            uintptr_t ptrVal = reinterpret_cast<uintptr_t>(this);
            uint32_t lo = static_cast<uint32_t>(ptrVal & 0xFFFFFFFF);
            uint32_t hi = static_cast<uint32_t>((ptrVal >> 32) & 0xFFFFFFFF);
            makecontext(&context, reinterpret_cast<void(*)()>(fiberTrampoline), 2, lo, hi);
#endif
        }

        ~Fiber() {
#if defined(_WIN32)
            if (fiberHandle != nullptr) {
                DeleteFiber(fiberHandle);
            }
#endif
        }

        void yield(YieldReason reason) {
            lastYieldReason = reason;
#if defined(_WIN32)
            SwitchToFiber(callerFiber);
#else
            swapcontext(&context, callerContext);
#endif
        }

        void runLoop() {
            while (!system->stop_.load(std::memory_order_relaxed)) {
                if (currentJob.func != nullptr) {
                    currentJob.func(currentJob.arg);
                    if (currentJob.counter != nullptr) {
                        currentJob.counter->fetch_sub(1, std::memory_order_acq_rel);
                    }
                    currentJob = {};
                }
                yield(YieldReason::Finished);
            }
        }

#if defined(_WIN32)
        static void CALLBACK fiberTrampoline(LPVOID param) {
            Fiber* f = reinterpret_cast<Fiber*>(param);
            f->runLoop();
        }
#else
        static void fiberTrampoline(uint32_t lo, uint32_t hi) {
            uintptr_t ptrVal = (static_cast<uintptr_t>(hi) << 32) | static_cast<uintptr_t>(lo);
            Fiber* f = reinterpret_cast<Fiber*>(ptrVal);
            f->runLoop();
        }
#endif
    };

    explicit FiberJobSystem(size_t threadCount, size_t fiberPoolSize = DEFAULT_FIBER_POOL_SIZE)
        : fiberCount_(fiberPoolSize) {
        fibers_ = std::make_unique<Fiber[]>(fiberCount_);
        freeFibers_.reserve(fiberCount_);

        for (size_t i = 0; i < fiberCount_; ++i) {
            fibers_[i].init(i, this);
            freeFibers_.push_back(&fibers_[i]);
        }

        workers_.reserve(threadCount);
        for (size_t i = 0; i < threadCount; ++i) {
            workers_.emplace_back([this]() {
                workerLoop();
            });
        }
    }

    ~FiberJobSystem() {
        stop_.store(true, std::memory_order_relaxed);
        {
            std::unique_lock<std::mutex> lock(jobMutex_);
            jobCv_.notify_all();
        }
        for (auto& w : workers_) {
            if (w.joinable()) {
                w.join();
            }
        }
    }

    FiberJobSystem(const FiberJobSystem&) = delete;
    FiberJobSystem& operator=(const FiberJobSystem&) = delete;
    FiberJobSystem(FiberJobSystem&&) = delete;
    FiberJobSystem& operator=(FiberJobSystem&&) = delete;

    size_t workerCount() const noexcept {
        return workers_.size();
    }

    void submitJob(Job job) {
        {
            std::unique_lock<std::mutex> lock(jobMutex_);
            jobQueue_.push(job);
        }
        jobCv_.notify_one();
    }

    void submitJobs(std::span<const Job> jobs) {
        if (jobs.empty()) return;
        {
            std::unique_lock<std::mutex> lock(jobMutex_);
            for (const auto& j : jobs) {
                jobQueue_.push(j);
            }
        }
        if (jobs.size() == 1) {
            jobCv_.notify_one();
        } else {
            jobCv_.notify_all();
        }
    }

    void waitForCounter(std::atomic<uint32_t>* counter, uint32_t targetValue = 0) {
        if (counter->load(std::memory_order_acquire) <= targetValue) {
            return;
        }

        Fiber* self = t_currentFiber;
        if (self != nullptr) {
            self->waitingCounter = counter;
            self->targetValue = targetValue;
            self->yield(YieldReason::WaitingOnCounter);
        } else {
            // Main thread / caller wait helper
            while (counter->load(std::memory_order_acquire) > targetValue) {
                Job job;
                bool hadJob = false;
                {
                    std::unique_lock<std::mutex> lock(jobMutex_);
                    if (!jobQueue_.empty()) {
                        job = jobQueue_.front();
                        jobQueue_.pop();
                        hadJob = true;
                    }
                }
                if (hadJob) {
                    if (job.func != nullptr) job.func(job.arg);
                    if (job.counter != nullptr) job.counter->fetch_sub(1, std::memory_order_acq_rel);
                } else {
                    std::this_thread::yield();
                }
            }
        }
    }

    // Workload: Wavefront
    void runWavefront(
        size_t taskCount,
        size_t iterations,
        std::atomic<size_t>& counter,
        std::vector<double>& outSamples
    ) {
        outSamples.reserve(iterations);
        std::vector<Job> leafJobs(taskCount - 2);

        for (size_t it = 0; it < iterations; ++it) {
            counter.store(0, std::memory_order_relaxed);
            std::atomic<uint32_t> leafCounter{static_cast<uint32_t>(taskCount - 2)};

            auto t0 = Clock::now();

            // 1. Root task runs inline or as first job
            counter.fetch_add(1, std::memory_order_relaxed);

            // 2. Leaf tasks
            for (size_t i = 0; i < taskCount - 2; ++i) {
                leafJobs[i] = Job{
                    .func = [](void* p) noexcept {
                        reinterpret_cast<std::atomic<size_t>*>(p)->fetch_add(1, std::memory_order_relaxed);
                    },
                    .arg = &counter,
                    .counter = &leafCounter
                };
            }
            submitJobs(leafJobs);

            // Wait for all leaves
            waitForCounter(&leafCounter, 0);

            // 3. Join task
            counter.fetch_add(1, std::memory_order_relaxed);

            auto t1 = Clock::now();
            outSamples.push_back(std::chrono::duration_cast<DurationUs>(t1 - t0).count());
        }
    }

    // Workload: Pipeline
    void runPipeline(
        size_t stages,
        size_t tasksPerStage,
        size_t iterations,
        std::atomic<size_t>& counter,
        std::vector<double>& outSamples
    ) {
        outSamples.reserve(iterations);
        std::vector<Job> stageJobs(tasksPerStage);

        for (size_t it = 0; it < iterations; ++it) {
            counter.store(0, std::memory_order_relaxed);
            auto t0 = Clock::now();

            for (size_t s = 0; s < stages; ++s) {
                std::atomic<uint32_t> stageCounter{static_cast<uint32_t>(tasksPerStage)};
                for (size_t t = 0; t < tasksPerStage; ++t) {
                    stageJobs[t] = Job{
                        .func = [](void* p) noexcept {
                            reinterpret_cast<std::atomic<size_t>*>(p)->fetch_add(1, std::memory_order_relaxed);
                        },
                        .arg = &counter,
                        .counter = &stageCounter
                    };
                }
                submitJobs(stageJobs);
                waitForCounter(&stageCounter, 0);
            }

            auto t1 = Clock::now();
            outSamples.push_back(std::chrono::duration_cast<DurationUs>(t1 - t0).count());
        }
    }

    // Workload: Parallel-For
    void runParallelFor(
        size_t itemCount,
        size_t batchSize,
        size_t iterations,
        std::vector<uint32_t>& data,
        std::vector<double>& outSamples
    ) {
        const size_t chunks = (itemCount + batchSize - 1) / batchSize;
        struct ChunkData {
            std::vector<uint32_t>* dataPtr;
            size_t start;
            size_t end;
        };

        std::vector<ChunkData> chunkContexts(chunks);
        std::vector<Job> chunkJobs(chunks);

        for (size_t c = 0; c < chunks; ++c) {
            chunkContexts[c].dataPtr = &data;
            chunkContexts[c].start = c * batchSize;
            chunkContexts[c].end = std::min(chunkContexts[c].start + batchSize, itemCount);
        }

        outSamples.reserve(iterations);
        for (size_t it = 0; it < iterations; ++it) {
            std::atomic<uint32_t> counter{static_cast<uint32_t>(chunks)};
            auto t0 = Clock::now();

            for (size_t c = 0; c < chunks; ++c) {
                chunkJobs[c] = Job{
                    .func = [](void* p) noexcept {
                        auto* cd = reinterpret_cast<ChunkData*>(p);
                        for (size_t i = cd->start; i < cd->end; ++i) {
                            (*cd->dataPtr)[i] = static_cast<uint32_t>(i * 3 + 1);
                        }
                    },
                    .arg = &chunkContexts[c],
                    .counter = &counter
                };
            }

            submitJobs(chunkJobs);
            waitForCounter(&counter, 0);

            auto t1 = Clock::now();
            outSamples.push_back(std::chrono::duration_cast<DurationUs>(t1 - t0).count());
        }
    }

private:
    static inline thread_local Fiber* t_currentFiber{nullptr};
#if defined(_WIN32)
    static inline thread_local LPVOID t_workerFiber{nullptr};
#endif

    void switchContextToFiber(Fiber* fiber) {
        t_currentFiber = fiber;
        fiber->state = FiberState::Running;
#if defined(_WIN32)
        fiber->callerFiber = t_workerFiber;
        SwitchToFiber(fiber->fiberHandle);
#else
        ucontext_t workerContext;
        fiber->callerContext = &workerContext;
        swapcontext(&workerContext, &fiber->context);
        fiber->callerContext = nullptr;
#endif
        t_currentFiber = nullptr;
    }

    void workerLoop() {
#if defined(_WIN32)
        t_workerFiber = ConvertThreadToFiber(nullptr);
#endif

        while (!stop_.load(std::memory_order_relaxed)) {
            // 1. Check waiting fibers
            Fiber* readyWaitingFiber = nullptr;
            {
                std::lock_guard<std::mutex> lock(fiberMutex_);
                for (auto it = waitingFibers_.begin(); it != waitingFibers_.end(); ++it) {
                    Fiber* f = *it;
                    if (f->waitingCounter != nullptr &&
                        f->waitingCounter->load(std::memory_order_acquire) <= f->targetValue) {
                        readyWaitingFiber = f;
                        waitingFibers_.erase(it);
                        break;
                    }
                }
            }

            if (readyWaitingFiber != nullptr) {
                switchContextToFiber(readyWaitingFiber);
                handleYieldedFiber(readyWaitingFiber);
                continue;
            }

            // 2. Fetch and run jobs
            Job job;
            bool hasJob = false;
            {
                std::unique_lock<std::mutex> lock(jobMutex_);
                if (!jobQueue_.empty()) {
                    job = jobQueue_.front();
                    jobQueue_.pop();
                    hasJob = true;
                }
            }

            if (hasJob) {
                Fiber* fiber = acquireFreeFiber();
                if (fiber != nullptr) {
                    fiber->currentJob = job;
                    switchContextToFiber(fiber);
                    handleYieldedFiber(fiber);
                } else {
                    // Pool depleted: run inline
                    if (job.func != nullptr) job.func(job.arg);
                    if (job.counter != nullptr) job.counter->fetch_sub(1, std::memory_order_acq_rel);
                }
                continue;
            }

            // 3. Idle wait
            {
                std::unique_lock<std::mutex> lock(jobMutex_);
                jobCv_.wait_for(lock, std::chrono::microseconds(50), [this]() {
                    return stop_.load(std::memory_order_relaxed) || !jobQueue_.empty();
                });
            }
        }

#if defined(_WIN32)
        ConvertFiberToThread();
#endif
    }

    void handleYieldedFiber(Fiber* fiber) {
        if (fiber->lastYieldReason == YieldReason::Finished) {
            std::lock_guard<std::mutex> lock(fiberMutex_);
            fiber->state = FiberState::Free;
            fiber->waitingCounter = nullptr;
            freeFibers_.push_back(fiber);
        } else if (fiber->lastYieldReason == YieldReason::WaitingOnCounter) {
            std::lock_guard<std::mutex> lock(fiberMutex_);
            fiber->state = FiberState::Waiting;
            waitingFibers_.push_back(fiber);
        }
    }

    Fiber* acquireFreeFiber() {
        std::lock_guard<std::mutex> lock(fiberMutex_);
        if (freeFibers_.empty()) {
            return nullptr;
        }
        Fiber* f = freeFibers_.back();
        freeFibers_.pop_back();
        return f;
    }

    size_t fiberCount_{0};
    std::unique_ptr<Fiber[]> fibers_;
    std::vector<Fiber*> freeFibers_;
    std::vector<Fiber*> waitingFibers_;
    std::mutex fiberMutex_;

    std::queue<Job> jobQueue_;
    std::mutex jobMutex_;
    std::condition_variable jobCv_;

    std::vector<std::thread> workers_;
    std::atomic<bool> stop_{false};
};

// =============================================================================
// 4. Modern C++20 Stackless Coroutine Task System
// =============================================================================

class CoroutineTaskSystem {
public:
    struct CoroTask {
        struct promise_type {
            CoroTask get_return_object() noexcept {
                return CoroTask{std::coroutine_handle<promise_type>::from_promise(*this)};
            }
            std::suspend_always initial_suspend() noexcept { return {}; }
            std::suspend_always final_suspend() noexcept { return {}; }
            void return_void() noexcept {}
            void unhandled_exception() noexcept { std::terminate(); }
        };

        std::coroutine_handle<promise_type> handle{nullptr};

        CoroTask() noexcept = default;
        explicit CoroTask(std::coroutine_handle<promise_type> h) noexcept : handle(h) {}
        ~CoroTask() {
            if (handle) {
                handle.destroy();
            }
        }

        CoroTask(const CoroTask&) = delete;
        CoroTask& operator=(const CoroTask&) = delete;
        CoroTask(CoroTask&& other) noexcept : handle(other.handle) {
            other.handle = nullptr;
        }
        CoroTask& operator=(CoroTask&& other) noexcept {
            if (this != &other) {
                if (handle) handle.destroy();
                handle = other.handle;
                other.handle = nullptr;
            }
            return *this;
        }

        void resume() {
            if (handle && !handle.done()) {
                handle.resume();
            }
        }

        bool done() const noexcept {
            return !handle || handle.done();
        }
    };

    struct CounterAwaiter {
        CoroutineTaskSystem* system;
        std::atomic<uint32_t>* counter;
        uint32_t targetValue{0};

        bool await_ready() const noexcept {
            return counter->load(std::memory_order_acquire) <= targetValue;
        }

        void await_suspend(std::coroutine_handle<> h) noexcept {
            system->registerWaitingCoroutine(h, counter, targetValue);
        }

        void await_resume() const noexcept {}
    };

    struct YieldAwaiter {
        CoroutineTaskSystem* system;

        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h) noexcept {
            system->enqueueResumption(h);
        }
        void await_resume() const noexcept {}
    };

    explicit CoroutineTaskSystem(size_t threadCount) {
        workers_.reserve(threadCount);
        for (size_t i = 0; i < threadCount; ++i) {
            workers_.emplace_back([this]() {
                workerLoop();
            });
        }
    }

    ~CoroutineTaskSystem() {
        stop_.store(true, std::memory_order_relaxed);
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueCv_.notify_all();
        }
        for (auto& w : workers_) {
            if (w.joinable()) {
                w.join();
            }
        }
    }

    CoroutineTaskSystem(const CoroutineTaskSystem&) = delete;
    CoroutineTaskSystem& operator=(const CoroutineTaskSystem&) = delete;
    CoroutineTaskSystem(CoroutineTaskSystem&&) = delete;
    CoroutineTaskSystem& operator=(CoroutineTaskSystem&&) = delete;

    size_t workerCount() const noexcept {
        return workers_.size();
    }

    void enqueueResumption(std::coroutine_handle<> h) {
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            readyQueue_.push(h);
        }
        queueCv_.notify_one();
    }

    void registerWaitingCoroutine(std::coroutine_handle<> h, std::atomic<uint32_t>* counter, uint32_t targetValue) {
        {
            std::lock_guard<std::mutex> lock(waitingMutex_);
            waitingCoros_.push_back(WaitingItem{
                .handle = h,
                .counter = counter,
                .targetValue = targetValue
            });
        }
        queueCv_.notify_one();
    }

    void waitForCounter(std::atomic<uint32_t>* counter, uint32_t targetValue = 0) {
        while (counter->load(std::memory_order_acquire) > targetValue) {
            std::coroutine_handle<> h;
            bool hasHandle = false;
            {
                std::unique_lock<std::mutex> lock(queueMutex_);
                if (!readyQueue_.empty()) {
                    h = readyQueue_.front();
                    readyQueue_.pop();
                    hasHandle = true;
                }
            }
            if (hasHandle) {
                h.resume();
            } else {
                std::this_thread::yield();
            }
        }
    }

    // Workload: Wavefront
    void runWavefront(
        size_t taskCount,
        size_t iterations,
        std::atomic<size_t>& counter,
        std::vector<double>& outSamples
    ) {
        outSamples.reserve(iterations);

        auto makeLeafCoro = [](std::atomic<size_t>* c, std::atomic<uint32_t>* completion) -> CoroTask {
            c->fetch_add(1, std::memory_order_relaxed);
            completion->fetch_sub(1, std::memory_order_acq_rel);
            co_return;
        };

        for (size_t it = 0; it < iterations; ++it) {
            counter.store(0, std::memory_order_relaxed);
            std::atomic<uint32_t> leafCounter{static_cast<uint32_t>(taskCount - 2)};

            auto t0 = Clock::now();

            // 1. Root task
            counter.fetch_add(1, std::memory_order_relaxed);

            // 2. Leaf coroutines
            for (size_t i = 0; i < taskCount - 2; ++i) {
                auto task = makeLeafCoro(&counter, &leafCounter);
                auto h = task.handle;
                task.handle = nullptr;
                enqueueResumption(h);
            }

            // Wait for leaves
            waitForCounter(&leafCounter, 0);

            // 3. Join task
            counter.fetch_add(1, std::memory_order_relaxed);

            auto t1 = Clock::now();
            outSamples.push_back(std::chrono::duration_cast<DurationUs>(t1 - t0).count());
        }
    }

    // Workload: Pipeline
    void runPipeline(
        size_t stages,
        size_t tasksPerStage,
        size_t iterations,
        std::atomic<size_t>& counter,
        std::vector<double>& outSamples
    ) {
        outSamples.reserve(iterations);

        auto makeStageCoro = [](std::atomic<size_t>* c, std::atomic<uint32_t>* completion) -> CoroTask {
            c->fetch_add(1, std::memory_order_relaxed);
            completion->fetch_sub(1, std::memory_order_acq_rel);
            co_return;
        };

        for (size_t it = 0; it < iterations; ++it) {
            counter.store(0, std::memory_order_relaxed);
            auto t0 = Clock::now();

            for (size_t s = 0; s < stages; ++s) {
                std::atomic<uint32_t> stageCounter{static_cast<uint32_t>(tasksPerStage)};
                for (size_t t = 0; t < tasksPerStage; ++t) {
                    auto task = makeStageCoro(&counter, &stageCounter);
                    auto h = task.handle;
                    task.handle = nullptr;
                    enqueueResumption(h);
                }
                waitForCounter(&stageCounter, 0);
            }

            auto t1 = Clock::now();
            outSamples.push_back(std::chrono::duration_cast<DurationUs>(t1 - t0).count());
        }
    }

    // Workload: Parallel-For
    void runParallelFor(
        size_t itemCount,
        size_t batchSize,
        size_t iterations,
        std::vector<uint32_t>& data,
        std::vector<double>& outSamples
    ) {
        const size_t chunks = (itemCount + batchSize - 1) / batchSize;

        auto makeChunkCoro = [](std::vector<uint32_t>* d, size_t start, size_t end, std::atomic<uint32_t>* completion) -> CoroTask {
            for (size_t i = start; i < end; ++i) {
                (*d)[i] = static_cast<uint32_t>(i * 3 + 1);
            }
            completion->fetch_sub(1, std::memory_order_acq_rel);
            co_return;
        };

        outSamples.reserve(iterations);
        for (size_t it = 0; it < iterations; ++it) {
            std::atomic<uint32_t> counter{static_cast<uint32_t>(chunks)};
            auto t0 = Clock::now();

            for (size_t c = 0; c < chunks; ++c) {
                size_t start = c * batchSize;
                size_t end = std::min(start + batchSize, itemCount);
                auto task = makeChunkCoro(&data, start, end, &counter);
                auto h = task.handle;
                task.handle = nullptr;
                enqueueResumption(h);
            }

            waitForCounter(&counter, 0);

            auto t1 = Clock::now();
            outSamples.push_back(std::chrono::duration_cast<DurationUs>(t1 - t0).count());
        }
    }

private:
    struct WaitingItem {
        std::coroutine_handle<> handle;
        std::atomic<uint32_t>* counter;
        uint32_t targetValue;
    };

    void workerLoop() {
        while (!stop_.load(std::memory_order_relaxed)) {
            // 1. Check waiting coroutines
            {
                std::lock_guard<std::mutex> lock(waitingMutex_);
                for (auto it = waitingCoros_.begin(); it != waitingCoros_.end();) {
                    if (it->counter->load(std::memory_order_acquire) <= it->targetValue) {
                        enqueueResumption(it->handle);
                        it = waitingCoros_.erase(it);
                    } else {
                        ++it;
                    }
                }
            }

            // 2. Fetch and resume ready coroutines
            std::coroutine_handle<> h;
            bool hasHandle = false;
            {
                std::unique_lock<std::mutex> lock(queueMutex_);
                if (!readyQueue_.empty()) {
                    h = readyQueue_.front();
                    readyQueue_.pop();
                    hasHandle = true;
                }
            }

            if (hasHandle) {
                if (h && !h.done()) {
                    h.resume();
                    if (h.done()) {
                        h.destroy();
                    }
                }
                continue;
            }

            // 3. Idle wait
            {
                std::unique_lock<std::mutex> lock(queueMutex_);
                queueCv_.wait_for(lock, std::chrono::microseconds(50), [this]() {
                    return stop_.load(std::memory_order_relaxed) || !readyQueue_.empty();
                });
            }
        }
    }

    std::queue<std::coroutine_handle<>> readyQueue_;
    std::mutex queueMutex_;
    std::condition_variable queueCv_;

    std::vector<WaitingItem> waitingCoros_;
    std::mutex waitingMutex_;

    std::vector<std::thread> workers_;
    std::atomic<bool> stop_{false};
};

} // namespace lf::comparison

#if defined(__APPLE__) || defined(__clang__)
#pragma clang diagnostic pop
#endif
