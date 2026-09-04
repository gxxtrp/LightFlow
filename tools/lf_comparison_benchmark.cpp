#if defined(__APPLE__)
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 600
#endif
#endif

#include <lightflow/lightflow.hpp>
#include "comparison_baselines.hpp"

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
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__APPLE__)
    #include <mach/mach.h>
    #include <sys/resource.h>
    #include <sys/mman.h>
    #include <unistd.h>
#elif defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>
    #include <psapi.h>
#else // Linux / POSIX
    #include <sys/resource.h>
    #include <sys/mman.h>
    #include <unistd.h>
#endif

// =============================================================================
// Low-Level Virtual Memory Allocator for BlockPool (LF_DISABLE_PLATFORM_ALLOCATOR)
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

// =============================================================================
// Global Heap Allocation Tracker
// =============================================================================

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

// =============================================================================
// Peak Resident Set Size (RSS) Measurement
// =============================================================================

inline size_t getPeakRSSBytes() noexcept {
#if defined(__APPLE__)
    struct rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        return static_cast<size_t>(usage.ru_maxrss); // ru_maxrss is in bytes on macOS
    }
    return 0;
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS info{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &info, sizeof(info))) {
        return static_cast<size_t>(info.PeakWorkingSetSize);
    }
    return 0;
#else // Linux / POSIX
    struct rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        return static_cast<size_t>(usage.ru_maxrss) * 1024; // ru_maxrss is in KB on Linux
    }
    return 0;
#endif
}

inline std::string getPlatformString() {
#if defined(__APPLE__)
    #if defined(__arm64__) || defined(__aarch64__)
        return "macOS (Apple Silicon)";
    #else
        return "macOS (x86_64)";
    #endif
#elif defined(_WIN32)
    #if defined(_M_ARM64)
        return "Windows (ARM64)";
    #else
        return "Windows (x86_64)";
    #endif
#elif defined(__linux__)
    #if defined(__aarch64__)
        return "Linux (AArch64)";
    #else
        return "Linux (x86_64)";
    #endif
#else
    return "Unknown Platform";
#endif
}

} // namespace lf::bench

// =============================================================================
// Global Operator New & Delete Overloads for Allocation Tracking
// =============================================================================

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
// Benchmark Data Models & Output Formatting
// =============================================================================

namespace lf::bench {

using Clock = std::chrono::high_resolution_clock;
using DurationUs = std::chrono::duration<double, std::micro>;

struct FrameworkMetric {
    std::string frameworkName;
    lf::comparison::LatencyStats stats;
    size_t allocations{0};
    size_t allocatedBytes{0};
    size_t peakRssBytes{0};
    double speedup{1.0};
    bool isReference{false};
};

struct WorkloadResult {
    std::string workloadName;
    std::string topologyDescription;
    size_t taskCount{0};
    size_t iterations{0};
    size_t threadCount{0};
    std::vector<FrameworkMetric> metrics;
};

inline std::string formatBytes(size_t bytes) {
    if (bytes == 0) {
        return "0 B";
    }
    char buf[64];
    if (bytes >= 1024 * 1024) {
        double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
        std::snprintf(buf, sizeof(buf), "%.1f MB", mb);
    } else if (bytes >= 1024) {
        double kb = static_cast<double>(bytes) / 1024.0;
        std::snprintf(buf, sizeof(buf), "%.1f KB", kb);
    } else {
        std::snprintf(buf, sizeof(buf), "%zu B", bytes);
    }
    return std::string(buf);
}

inline std::string formatAllocations(size_t allocCount, size_t allocBytes) {
    char buf[80];
    if (allocCount == 0) {
        std::snprintf(buf, sizeof(buf), "0 allocs (0 B)");
    } else {
        std::string bytesStr = formatBytes(allocBytes);
        std::snprintf(buf, sizeof(buf), "%zu allocs (%s)", allocCount, bytesStr.c_str());
    }
    return std::string(buf);
}

inline std::string formatLatency(double us) {
    char buf[32];
    if (us >= 1000.0) {
        std::snprintf(buf, sizeof(buf), "%.0f us", us);
    } else {
        std::snprintf(buf, sizeof(buf), "%.1f us", us);
    }
    return std::string(buf);
}

inline void printAsciiComparisonTable(
    const std::string& platformStr,
    const WorkloadResult& result
) {
    std::cout << "\n========================================================================================================================\n"
              << " LightFlow Architectural Benchmark: Multi-Paradigm Comparison\n"
              << " Platform: " << platformStr
              << " | Worker Threads: " << result.threadCount
              << " | Iterations: " << result.iterations << "\n"
              << "========================================================================================================================\n"
              << " Workload: " << result.topologyDescription << " (" << result.taskCount << " tasks)\n"
              << "+-------------------------------+---------------+---------------+---------------+-------------------------+------------+------------------+\n"
              << "| " << std::left << std::setw(29) << "Framework"
              << " | " << std::right << std::setw(13) << "Min Latency"
              << " | " << std::setw(13) << "P50 Latency"
              << " | " << std::setw(13) << "P99 Latency"
              << " | " << std::setw(23) << "Allocations"
              << " | " << std::setw(10) << "Peak RSS"
              << " | " << std::setw(16) << "Speedup"
              << " |\n"
              << "+-------------------------------+---------------+---------------+---------------+-------------------------+------------+------------------+\n";

    double lfSpeedup = 1.0;
    double lfP50Speedup = 1.0;
    const FrameworkMetric* refMetric = nullptr;
    const FrameworkMetric* lfMetric = nullptr;

    for (const auto& m : result.metrics) {
        if (m.isReference) refMetric = &m;
        if (m.frameworkName.find("LightFlow") != std::string::npos) lfMetric = &m;
    }

    if (refMetric && lfMetric && lfMetric->stats.meanUs > 0.0 && refMetric->stats.meanUs > 0.0) {
        lfSpeedup = refMetric->stats.meanUs / lfMetric->stats.meanUs;
        lfP50Speedup = refMetric->stats.p50Us / lfMetric->stats.p50Us;
    }

    for (const auto& m : result.metrics) {
        char speedupBuf[32];
        if (m.isReference) {
            std::snprintf(speedupBuf, sizeof(speedupBuf), "1.00x (ref)");
        } else if (m.speedup >= 1.0) {
            std::snprintf(speedupBuf, sizeof(speedupBuf), "%.2fx", m.speedup);
        } else {
            std::snprintf(speedupBuf, sizeof(speedupBuf), "%.2fx slower", 1.0 / m.speedup);
        }

        std::string allocStr = formatAllocations(m.allocations, m.allocatedBytes);
        std::string rssStr = formatBytes(m.peakRssBytes);

        std::cout << "| " << std::left << std::setw(29) << m.frameworkName
                  << " | " << std::right << std::setw(13) << formatLatency(m.stats.minUs)
                  << " | " << std::setw(13) << formatLatency(m.stats.p50Us)
                  << " | " << std::setw(13) << formatLatency(m.stats.p99Us)
                  << " | " << std::setw(23) << allocStr
                  << " | " << std::setw(10) << rssStr
                  << " | " << std::setw(16) << speedupBuf
                  << " |\n";
    }

    std::cout << "+-------------------------------+---------------+---------------+---------------+-------------------------+------------+------------------+\n";
    if (lfMetric && refMetric) {
        std::cout << " >>> LightFlow is " << std::fixed << std::setprecision(2) << lfSpeedup
                  << "x faster than Classic ThreadPool (P50 speedup: " << lfP50Speedup << "x) <<<\n";
    }
    std::cout << "========================================================================================================================\n\n";
}

inline void writeJsonReport(
    const std::string& path,
    const std::string& platformStr,
    size_t threadCount,
    std::span<const WorkloadResult> results
) {
    std::ofstream out(path);
    if (!out.is_open()) {
        std::cerr << "Error: Unable to open JSON file for writing: " << path << "\n";
        return;
    }

    const auto nowSec = std::chrono::duration_cast<std::chrono::seconds>(Clock::now().time_since_epoch()).count();

    out << "{\n"
        << "  \"meta\": {\n"
        << "    \"engine\": \"LightFlow Task Graph Engine\",\n"
        << "    \"platform\": \"" << platformStr << "\",\n"
        << "    \"worker_threads\": " << threadCount << ",\n"
        << "    \"timestamp\": " << nowSec << "\n"
        << "  },\n"
        << "  \"workloads\": [\n";

    for (size_t w = 0; w < results.size(); ++w) {
        const auto& wl = results[w];
        out << "    {\n"
            << "      \"name\": \"" << wl.workloadName << "\",\n"
            << "      \"description\": \"" << wl.topologyDescription << "\",\n"
            << "      \"task_count\": " << wl.taskCount << ",\n"
            << "      \"iterations\": " << wl.iterations << ",\n"
            << "      \"thread_count\": " << wl.threadCount << ",\n"
            << "      \"frameworks\": [\n";

        for (size_t f = 0; f < wl.metrics.size(); ++f) {
            const auto& m = wl.metrics[f];
            double peakMb = static_cast<double>(m.peakRssBytes) / (1024.0 * 1024.0);
            out << "        {\n"
                << "          \"framework\": \"" << m.frameworkName << "\",\n"
                << "          \"min_us\": " << std::fixed << std::setprecision(2) << m.stats.minUs << ",\n"
                << "          \"mean_us\": " << std::fixed << std::setprecision(2) << m.stats.meanUs << ",\n"
                << "          \"p50_us\": " << std::fixed << std::setprecision(2) << m.stats.p50Us << ",\n"
                << "          \"p95_us\": " << std::fixed << std::setprecision(2) << m.stats.p95Us << ",\n"
                << "          \"p99_us\": " << std::fixed << std::setprecision(2) << m.stats.p99Us << ",\n"
                << "          \"max_us\": " << std::fixed << std::setprecision(2) << m.stats.maxUs << ",\n"
                << "          \"allocations\": " << m.allocations << ",\n"
                << "          \"allocated_bytes\": " << m.allocatedBytes << ",\n"
                << "          \"peak_rss_bytes\": " << m.peakRssBytes << ",\n"
                << "          \"peak_rss_mb\": " << std::fixed << std::setprecision(2) << peakMb << ",\n"
                << "          \"speedup_vs_classic\": " << std::fixed << std::setprecision(2) << m.speedup << ",\n"
                << "          \"is_reference\": " << (m.isReference ? "true" : "false") << "\n"
                << "        }" << (f + 1 < wl.metrics.size() ? ",\n" : "\n");
        }

        out << "      ]\n"
            << "    }" << (w + 1 < results.size() ? ",\n" : "\n");
    }

    out << "  ]\n"
        << "}\n";

    std::cout << ">>> Successfully exported structured JSON results to: " << path << " <<<\n\n";
}

// =============================================================================
// Workload 1: 50,000-Task Wavefront (Fan-Out & Fan-In DAG)
// =============================================================================

inline WorkloadResult benchmarkWavefront(size_t threadCount, size_t iterations) {
    using namespace lf::comparison;

    constexpr size_t TASK_COUNT = 50000;
    WorkloadResult result{
        .workloadName = "wavefront",
        .topologyDescription = "50,000-Task Wavefront (Fan-Out & Fan-In)",
        .taskCount = TASK_COUNT,
        .iterations = iterations,
        .threadCount = threadCount,
        .metrics = {}
    };

    // 1. LightFlow
    {
        lf::SchedulerConfig lfConfig{
            .workerCount = static_cast<lf::u32>(threadCount),
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

        // Warm-up
        lfScheduler.runAndWait(lfGraph);

        std::vector<double> lfSamples;
        lfSamples.reserve(iterations);

        AllocationTracker::reset();
        AllocationTracker::enable();

        for (size_t it = 0; it < iterations; ++it) {
            lfCounter.store(0, std::memory_order_relaxed);
            auto t0 = Clock::now();
            lfScheduler.runAndWait(lfGraph);
            auto t1 = Clock::now();
            lfSamples.push_back(std::chrono::duration_cast<DurationUs>(t1 - t0).count());
        }

        AllocationTracker::disable();
        result.metrics.push_back({
            .frameworkName = "LightFlow (Stackless)",
            .stats = LatencyStats::compute(lfSamples),
            .allocations = AllocationTracker::allocations(),
            .allocatedBytes = AllocationTracker::bytes(),
            .peakRssBytes = getPeakRSSBytes(),
            .speedup = 1.0,
            .isReference = false
        });
    }

    // 2. Taskflow v3.8.0
#if defined(LF_HAS_TASKFLOW) && LF_HAS_TASKFLOW
    {
        TaskflowRunner tfRunner(threadCount);
        std::atomic<size_t> tfCounter{0};
        std::vector<double> tfSamples;

        AllocationTracker::reset();
        AllocationTracker::enable();

        tfRunner.runWavefront(TASK_COUNT, iterations, tfCounter, tfSamples);

        AllocationTracker::disable();
        result.metrics.push_back({
            .frameworkName = "Taskflow v3.8.0",
            .stats = LatencyStats::compute(tfSamples),
            .allocations = AllocationTracker::allocations(),
            .allocatedBytes = AllocationTracker::bytes(),
            .peakRssBytes = getPeakRSSBytes(),
            .speedup = 1.0,
            .isReference = false
        });
    }
#endif

    // 3. Stackful Fiber Job System (Naughty Dog GDC 2015 Model)
    {
        FiberJobSystem fiberSystem(threadCount);
        std::atomic<size_t> fiberCounter{0};
        std::vector<double> fiberSamples;

        AllocationTracker::reset();
        AllocationTracker::enable();

        fiberSystem.runWavefront(TASK_COUNT, iterations, fiberCounter, fiberSamples);

        AllocationTracker::disable();
        result.metrics.push_back({
            .frameworkName = "Fiber Job System (GDC 2015)",
            .stats = LatencyStats::compute(fiberSamples),
            .allocations = AllocationTracker::allocations(),
            .allocatedBytes = AllocationTracker::bytes(),
            .peakRssBytes = getPeakRSSBytes(),
            .speedup = 1.0,
            .isReference = false
        });
    }

    // 4. C++20 Stackless Coroutines
    {
        CoroutineTaskSystem coroSystem(threadCount);
        std::atomic<size_t> coroCounter{0};
        std::vector<double> coroSamples;

        AllocationTracker::reset();
        AllocationTracker::enable();

        coroSystem.runWavefront(TASK_COUNT, iterations, coroCounter, coroSamples);

        AllocationTracker::disable();
        result.metrics.push_back({
            .frameworkName = "C++20 Coroutines (Stackless)",
            .stats = LatencyStats::compute(coroSamples),
            .allocations = AllocationTracker::allocations(),
            .allocatedBytes = AllocationTracker::bytes(),
            .peakRssBytes = getPeakRSSBytes(),
            .speedup = 1.0,
            .isReference = false
        });
    }

    // 5. Classic ThreadPool Baseline
    {
        ClassicThreadPool classicPool(threadCount);
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

        // Warm-up
        classicGraph.reset();
        classicGraph.executeAndWait(classicPool);

        AllocationTracker::reset();
        AllocationTracker::enable();

        std::vector<double> classicSamples;
        classicSamples.reserve(iterations);
        for (size_t it = 0; it < iterations; ++it) {
            classicCounter.store(0, std::memory_order_relaxed);
            classicGraph.reset();
            auto t0 = Clock::now();
            classicGraph.executeAndWait(classicPool);
            auto t1 = Clock::now();
            classicSamples.push_back(std::chrono::duration_cast<DurationUs>(t1 - t0).count());
        }

        AllocationTracker::disable();
        result.metrics.push_back({
            .frameworkName = "Classic ThreadPool",
            .stats = LatencyStats::compute(classicSamples),
            .allocations = AllocationTracker::allocations(),
            .allocatedBytes = AllocationTracker::bytes(),
            .peakRssBytes = getPeakRSSBytes(),
            .speedup = 1.0,
            .isReference = true
        });
    }

    // Compute speedup vs Classic ThreadPool
    const double classicMean = result.metrics.back().stats.meanUs;
    for (auto& m : result.metrics) {
        if (!m.isReference && m.stats.meanUs > 0.0 && classicMean > 0.0) {
            m.speedup = classicMean / m.stats.meanUs;
        }
    }

    return result;
}

// =============================================================================
// Workload 2: 50,000-Task Multi-Stage Pipeline (10x5,000 Tasks)
// =============================================================================

inline WorkloadResult benchmarkPipeline(size_t threadCount, size_t iterations) {
    using namespace lf::comparison;

    constexpr size_t STAGES = 10;
    constexpr size_t TASKS_PER_STAGE = 5000;
    constexpr size_t TOTAL_TASKS = STAGES * TASKS_PER_STAGE;

    WorkloadResult result{
        .workloadName = "pipeline",
        .topologyDescription = "50,000-Task Multi-Stage Pipeline (10x5,000 Tasks)",
        .taskCount = TOTAL_TASKS,
        .iterations = iterations,
        .threadCount = threadCount,
        .metrics = {}
    };

    // 1. LightFlow Multi-Stage Pipeline
    {
        lf::SchedulerConfig lfConfig{
            .workerCount = static_cast<lf::u32>(threadCount),
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

        // Warm-up
        lfScheduler.runAndWait(lfGraph);

        std::vector<double> lfSamples;
        lfSamples.reserve(iterations);

        AllocationTracker::reset();
        AllocationTracker::enable();

        for (size_t it = 0; it < iterations; ++it) {
            lfCounter.store(0, std::memory_order_relaxed);
            auto t0 = Clock::now();
            lfScheduler.runAndWait(lfGraph);
            auto t1 = Clock::now();
            lfSamples.push_back(std::chrono::duration_cast<DurationUs>(t1 - t0).count());
        }

        AllocationTracker::disable();
        result.metrics.push_back({
            .frameworkName = "LightFlow (Stackless)",
            .stats = LatencyStats::compute(lfSamples),
            .allocations = AllocationTracker::allocations(),
            .allocatedBytes = AllocationTracker::bytes(),
            .peakRssBytes = getPeakRSSBytes(),
            .speedup = 1.0,
            .isReference = false
        });
    }

    // 2. Taskflow v3.8.0
#if defined(LF_HAS_TASKFLOW) && LF_HAS_TASKFLOW
    {
        TaskflowRunner tfRunner(threadCount);
        std::atomic<size_t> tfCounter{0};
        std::vector<double> tfSamples;

        AllocationTracker::reset();
        AllocationTracker::enable();

        tfRunner.runPipeline(STAGES, TASKS_PER_STAGE, iterations, tfCounter, tfSamples);

        AllocationTracker::disable();
        result.metrics.push_back({
            .frameworkName = "Taskflow v3.8.0",
            .stats = LatencyStats::compute(tfSamples),
            .allocations = AllocationTracker::allocations(),
            .allocatedBytes = AllocationTracker::bytes(),
            .peakRssBytes = getPeakRSSBytes(),
            .speedup = 1.0,
            .isReference = false
        });
    }
#endif

    // 3. Stackful Fiber Job System
    {
        FiberJobSystem fiberSystem(threadCount);
        std::atomic<size_t> fiberCounter{0};
        std::vector<double> fiberSamples;

        AllocationTracker::reset();
        AllocationTracker::enable();

        fiberSystem.runPipeline(STAGES, TASKS_PER_STAGE, iterations, fiberCounter, fiberSamples);

        AllocationTracker::disable();
        result.metrics.push_back({
            .frameworkName = "Fiber Job System (GDC 2015)",
            .stats = LatencyStats::compute(fiberSamples),
            .allocations = AllocationTracker::allocations(),
            .allocatedBytes = AllocationTracker::bytes(),
            .peakRssBytes = getPeakRSSBytes(),
            .speedup = 1.0,
            .isReference = false
        });
    }

    // 4. C++20 Stackless Coroutines
    {
        CoroutineTaskSystem coroSystem(threadCount);
        std::atomic<size_t> coroCounter{0};
        std::vector<double> coroSamples;

        AllocationTracker::reset();
        AllocationTracker::enable();

        coroSystem.runPipeline(STAGES, TASKS_PER_STAGE, iterations, coroCounter, coroSamples);

        AllocationTracker::disable();
        result.metrics.push_back({
            .frameworkName = "C++20 Coroutines (Stackless)",
            .stats = LatencyStats::compute(coroSamples),
            .allocations = AllocationTracker::allocations(),
            .allocatedBytes = AllocationTracker::bytes(),
            .peakRssBytes = getPeakRSSBytes(),
            .speedup = 1.0,
            .isReference = false
        });
    }

    // 5. Classic Multi-Stage Pipeline
    {
        ClassicThreadPool classicPool(threadCount);
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

        // Warm-up
        classicGraph.reset();
        classicGraph.executeAndWait(classicPool);

        AllocationTracker::reset();
        AllocationTracker::enable();

        std::vector<double> classicSamples;
        classicSamples.reserve(iterations);
        for (size_t it = 0; it < iterations; ++it) {
            classicCounter.store(0, std::memory_order_relaxed);
            classicGraph.reset();
            auto t0 = Clock::now();
            classicGraph.executeAndWait(classicPool);
            auto t1 = Clock::now();
            classicSamples.push_back(std::chrono::duration_cast<DurationUs>(t1 - t0).count());
        }

        AllocationTracker::disable();
        result.metrics.push_back({
            .frameworkName = "Classic ThreadPool",
            .stats = LatencyStats::compute(classicSamples),
            .allocations = AllocationTracker::allocations(),
            .allocatedBytes = AllocationTracker::bytes(),
            .peakRssBytes = getPeakRSSBytes(),
            .speedup = 1.0,
            .isReference = true
        });
    }

    // Compute speedup vs Classic ThreadPool
    const double classicMean = result.metrics.back().stats.meanUs;
    for (auto& m : result.metrics) {
        if (!m.isReference && m.stats.meanUs > 0.0 && classicMean > 0.0) {
            m.speedup = classicMean / m.stats.meanUs;
        }
    }

    return result;
}

// =============================================================================
// Workload 3: 5,000,000-Item Parallel For Workload
// =============================================================================

inline WorkloadResult benchmarkParallelFor(size_t threadCount, size_t iterations) {
    using namespace lf::comparison;

    constexpr size_t ITEM_COUNT = 5000000;
    constexpr size_t BATCH_SIZE = 2048;

    WorkloadResult result{
        .workloadName = "parallel_for",
        .topologyDescription = "5,000,000-Item Parallel For Workload",
        .taskCount = ITEM_COUNT,
        .iterations = iterations,
        .threadCount = threadCount,
        .metrics = {}
    };

    // 1. LightFlow Parallel For
    {
        std::vector<uint32_t> lfData(ITEM_COUNT, 0);
        lf::SchedulerConfig lfConfig{
            .workerCount = static_cast<lf::u32>(threadCount),
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
        lfSamples.reserve(iterations);

        AllocationTracker::reset();
        AllocationTracker::enable();

        for (size_t it = 0; it < iterations; ++it) {
            auto t0 = Clock::now();
            lfScheduler.runAndWait(lfGraph);
            auto t1 = Clock::now();
            lfSamples.push_back(std::chrono::duration_cast<DurationUs>(t1 - t0).count());
        }

        AllocationTracker::disable();
        result.metrics.push_back({
            .frameworkName = "LightFlow (Stackless)",
            .stats = LatencyStats::compute(lfSamples),
            .allocations = AllocationTracker::allocations(),
            .allocatedBytes = AllocationTracker::bytes(),
            .peakRssBytes = getPeakRSSBytes(),
            .speedup = 1.0,
            .isReference = false
        });
    }

    // 2. Taskflow v3.8.0
#if defined(LF_HAS_TASKFLOW) && LF_HAS_TASKFLOW
    {
        std::vector<uint32_t> tfData(ITEM_COUNT, 0);
        TaskflowRunner tfRunner(threadCount);
        std::vector<double> tfSamples;

        AllocationTracker::reset();
        AllocationTracker::enable();

        tfRunner.runParallelFor(ITEM_COUNT, BATCH_SIZE, iterations, tfData, tfSamples);

        AllocationTracker::disable();
        result.metrics.push_back({
            .frameworkName = "Taskflow v3.8.0",
            .stats = LatencyStats::compute(tfSamples),
            .allocations = AllocationTracker::allocations(),
            .allocatedBytes = AllocationTracker::bytes(),
            .peakRssBytes = getPeakRSSBytes(),
            .speedup = 1.0,
            .isReference = false
        });
    }
#endif

    // 3. Stackful Fiber Job System
    {
        std::vector<uint32_t> fiberData(ITEM_COUNT, 0);
        FiberJobSystem fiberSystem(threadCount);
        std::vector<double> fiberSamples;

        AllocationTracker::reset();
        AllocationTracker::enable();

        fiberSystem.runParallelFor(ITEM_COUNT, BATCH_SIZE, iterations, fiberData, fiberSamples);

        AllocationTracker::disable();
        result.metrics.push_back({
            .frameworkName = "Fiber Job System (GDC 2015)",
            .stats = LatencyStats::compute(fiberSamples),
            .allocations = AllocationTracker::allocations(),
            .allocatedBytes = AllocationTracker::bytes(),
            .peakRssBytes = getPeakRSSBytes(),
            .speedup = 1.0,
            .isReference = false
        });
    }

    // 4. C++20 Stackless Coroutines
    {
        std::vector<uint32_t> coroData(ITEM_COUNT, 0);
        CoroutineTaskSystem coroSystem(threadCount);
        std::vector<double> coroSamples;

        AllocationTracker::reset();
        AllocationTracker::enable();

        coroSystem.runParallelFor(ITEM_COUNT, BATCH_SIZE, iterations, coroData, coroSamples);

        AllocationTracker::disable();
        result.metrics.push_back({
            .frameworkName = "C++20 Coroutines (Stackless)",
            .stats = LatencyStats::compute(coroSamples),
            .allocations = AllocationTracker::allocations(),
            .allocatedBytes = AllocationTracker::bytes(),
            .peakRssBytes = getPeakRSSBytes(),
            .speedup = 1.0,
            .isReference = false
        });
    }

    // 5. Classic ThreadPool
    {
        std::vector<uint32_t> classicData(ITEM_COUNT, 0);
        ClassicThreadPool classicPool(threadCount);
        const size_t chunks = (ITEM_COUNT + BATCH_SIZE - 1) / BATCH_SIZE;

        // Warm-up
        {
            std::atomic<size_t> remainingChunks{chunks};
            std::mutex mtx;
            std::condition_variable cv;
            bool done = false;
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
        }

        AllocationTracker::reset();
        AllocationTracker::enable();

        std::vector<double> classicSamples;
        classicSamples.reserve(iterations);
        for (size_t it = 0; it < iterations; ++it) {
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

        AllocationTracker::disable();
        result.metrics.push_back({
            .frameworkName = "Classic ThreadPool",
            .stats = LatencyStats::compute(classicSamples),
            .allocations = AllocationTracker::allocations(),
            .allocatedBytes = AllocationTracker::bytes(),
            .peakRssBytes = getPeakRSSBytes(),
            .speedup = 1.0,
            .isReference = true
        });
    }

    // Compute speedup vs Classic ThreadPool
    const double classicMean = result.metrics.back().stats.meanUs;
    for (auto& m : result.metrics) {
        if (!m.isReference && m.stats.meanUs > 0.0 && classicMean > 0.0) {
            m.speedup = classicMean / m.stats.meanUs;
        }
    }

    return result;
}

// =============================================================================
// Workload 4: Steady-State Frame Loop (100 Frames, 10,000 Tasks/Frame)
// =============================================================================

inline WorkloadResult benchmarkFrameLoop(size_t threadCount, size_t frameCount) {
    using namespace lf::comparison;

    constexpr size_t TASKS_PER_FRAME = 10000;
    WorkloadResult result{
        .workloadName = "frame_loop",
        .topologyDescription = "Steady-State Frame Loop (10,000 Tasks/Frame)",
        .taskCount = TASKS_PER_FRAME * frameCount,
        .iterations = frameCount,
        .threadCount = threadCount,
        .metrics = {}
    };

    // 1. LightFlow Steady-State Frame Loop
    {
        lf::SchedulerConfig lfConfig{
            .workerCount = static_cast<lf::u32>(threadCount),
            .initialDequeCapacity = 16384
        };
        lf::TaskScheduler lfScheduler(lfConfig);
        lf::TaskGraph lfGraph;
        std::atomic<size_t> lfCounter{0};

        lf::TaskHandle lfRoot = lfGraph.emplace([&lfCounter]() noexcept {
            lfCounter.fetch_add(1, std::memory_order_relaxed);
        });

        std::vector<lf::TaskHandle> lfLeaves;
        lfLeaves.reserve(TASKS_PER_FRAME - 2);
        for (size_t i = 0; i < TASKS_PER_FRAME - 2; ++i) {
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

        // Warm-up 3 frames
        for (int w = 0; w < 3; ++w) {
            lfScheduler.runAndWait(lfGraph);
        }

        std::vector<double> lfSamples;
        lfSamples.reserve(frameCount);

        AllocationTracker::reset();
        AllocationTracker::enable();

        for (size_t f = 0; f < frameCount; ++f) {
            auto t0 = Clock::now();
            lfScheduler.runAndWait(lfGraph);
            auto t1 = Clock::now();
            lfSamples.push_back(std::chrono::duration_cast<DurationUs>(t1 - t0).count());
        }

        AllocationTracker::disable();
        size_t lfAllocs = AllocationTracker::allocations();
        size_t lfBytes = AllocationTracker::bytes();

        // System invariant check: LightFlow must maintain 0 dynamic allocations in steady-state
        LF_ASSERT(lfAllocs == 0);

        result.metrics.push_back({
            .frameworkName = "LightFlow (Stackless)",
            .stats = LatencyStats::compute(lfSamples),
            .allocations = lfAllocs,
            .allocatedBytes = lfBytes,
            .peakRssBytes = getPeakRSSBytes(),
            .speedup = 1.0,
            .isReference = false
        });
    }

    // 2. Taskflow v3.8.0
#if defined(LF_HAS_TASKFLOW) && LF_HAS_TASKFLOW
    {
        TaskflowRunner tfRunner(threadCount);
        tf::Taskflow tf;
        std::atomic<size_t> tfCounter{0};

        auto root = tf.emplace([&tfCounter]() {
            tfCounter.fetch_add(1, std::memory_order_relaxed);
        });
        auto join = tf.emplace([&tfCounter]() {
            tfCounter.fetch_add(1, std::memory_order_relaxed);
        });

        std::vector<tf::Task> leaves;
        leaves.reserve(TASKS_PER_FRAME - 2);
        for (size_t i = 0; i < TASKS_PER_FRAME - 2; ++i) {
            auto leaf = tf.emplace([&tfCounter]() {
                tfCounter.fetch_add(1, std::memory_order_relaxed);
            });
            root.precede(leaf);
            leaf.precede(join);
            leaves.push_back(leaf);
        }

        // Warm-up 3 frames
        for (int w = 0; w < 3; ++w) {
            tfRunner.executor().run(tf).wait();
        }

        AllocationTracker::reset();
        AllocationTracker::enable();

        std::vector<double> tfSamples;
        tfSamples.reserve(frameCount);
        for (size_t f = 0; f < frameCount; ++f) {
            auto t0 = Clock::now();
            tfRunner.executor().run(tf).wait();
            auto t1 = Clock::now();
            tfSamples.push_back(std::chrono::duration_cast<DurationUs>(t1 - t0).count());
        }

        AllocationTracker::disable();
        result.metrics.push_back({
            .frameworkName = "Taskflow v3.8.0",
            .stats = LatencyStats::compute(tfSamples),
            .allocations = AllocationTracker::allocations(),
            .allocatedBytes = AllocationTracker::bytes(),
            .peakRssBytes = getPeakRSSBytes(),
            .speedup = 1.0,
            .isReference = false
        });
    }
#endif

    // 3. Stackful Fiber Job System
    {
        FiberJobSystem fiberSystem(threadCount);
        std::atomic<size_t> fiberCounter{0};
        std::vector<double> dummySamples;
        fiberSystem.runWavefront(TASKS_PER_FRAME, 3, fiberCounter, dummySamples);

        AllocationTracker::reset();
        AllocationTracker::enable();

        std::vector<double> fiberSamples;
        fiberSystem.runWavefront(TASKS_PER_FRAME, frameCount, fiberCounter, fiberSamples);

        AllocationTracker::disable();
        result.metrics.push_back({
            .frameworkName = "Fiber Job System (GDC 2015)",
            .stats = LatencyStats::compute(fiberSamples),
            .allocations = AllocationTracker::allocations(),
            .allocatedBytes = AllocationTracker::bytes(),
            .peakRssBytes = getPeakRSSBytes(),
            .speedup = 1.0,
            .isReference = false
        });
    }

    // 4. C++20 Stackless Coroutines
    {
        CoroutineTaskSystem coroSystem(threadCount);
        std::atomic<size_t> coroCounter{0};
        std::vector<double> dummySamples;
        coroSystem.runWavefront(TASKS_PER_FRAME, 3, coroCounter, dummySamples);

        AllocationTracker::reset();
        AllocationTracker::enable();

        std::vector<double> coroSamples;
        coroSystem.runWavefront(TASKS_PER_FRAME, frameCount, coroCounter, coroSamples);

        AllocationTracker::disable();
        result.metrics.push_back({
            .frameworkName = "C++20 Coroutines (Stackless)",
            .stats = LatencyStats::compute(coroSamples),
            .allocations = AllocationTracker::allocations(),
            .allocatedBytes = AllocationTracker::bytes(),
            .peakRssBytes = getPeakRSSBytes(),
            .speedup = 1.0,
            .isReference = false
        });
    }

    // 5. Classic ThreadPool
    {
        ClassicThreadPool classicPool(threadCount);
        ClassicTaskGraph classicGraph;
        std::atomic<size_t> classicCounter{0};

        auto* classicRoot = classicGraph.emplace([&classicCounter]() {
            classicCounter.fetch_add(1, std::memory_order_relaxed);
        });

        std::vector<ClassicTaskGraph::Node*> classicLeaves;
        classicLeaves.reserve(TASKS_PER_FRAME - 2);
        for (size_t i = 0; i < TASKS_PER_FRAME - 2; ++i) {
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

        // Warm-up 3 frames
        for (int w = 0; w < 3; ++w) {
            classicGraph.reset();
            classicGraph.executeAndWait(classicPool);
        }

        AllocationTracker::reset();
        AllocationTracker::enable();

        std::vector<double> classicSamples;
        classicSamples.reserve(frameCount);
        for (size_t f = 0; f < frameCount; ++f) {
            classicGraph.reset();
            auto t0 = Clock::now();
            classicGraph.executeAndWait(classicPool);
            auto t1 = Clock::now();
            classicSamples.push_back(std::chrono::duration_cast<DurationUs>(t1 - t0).count());
        }

        AllocationTracker::disable();
        result.metrics.push_back({
            .frameworkName = "Classic ThreadPool",
            .stats = LatencyStats::compute(classicSamples),
            .allocations = AllocationTracker::allocations(),
            .allocatedBytes = AllocationTracker::bytes(),
            .peakRssBytes = getPeakRSSBytes(),
            .speedup = 1.0,
            .isReference = true
        });
    }

    // Compute speedup vs Classic ThreadPool
    const double classicMean = result.metrics.back().stats.meanUs;
    for (auto& m : result.metrics) {
        if (!m.isReference && m.stats.meanUs > 0.0 && classicMean > 0.0) {
            m.speedup = classicMean / m.stats.meanUs;
        }
    }

    return result;
}

} // namespace lf::bench

// =============================================================================
// CLI Entry Point
// =============================================================================

int main(int argc, char* argv[]) {
    using namespace lf::bench;

    // Install virtual memory allocator callbacks for LightFlow BlockPool
    lf::MemoryCallbacks benchCallbacks{
        .alloc = &bench_alloc,
        .free = &bench_free,
        .user_data = nullptr
    };
    lf::BlockPool::set_global_callbacks(benchCallbacks);

    std::string workloadFilter = "all";
    size_t threadCount = std::max<size_t>(2, std::thread::hardware_concurrency());
    size_t iterations = 10;
    bool customIterations = false;
    std::string jsonOutputPath;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--workload" && i + 1 < argc) {
            workloadFilter = argv[++i];
        } else if (arg == "--threads" && i + 1 < argc) {
            threadCount = static_cast<size_t>(std::stoull(argv[++i]));
        } else if (arg == "--iterations" && i + 1 < argc) {
            iterations = static_cast<size_t>(std::stoull(argv[++i]));
            customIterations = true;
        } else if (arg == "--json" && i + 1 < argc) {
            jsonOutputPath = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "LightFlow Multi-Paradigm Comparison Benchmark CLI Tool\n\n"
                      << "Usage:\n"
                      << "  " << argv[0] << " [options]\n\n"
                      << "Options:\n"
                      << "  --workload <type>   Filter workload: 'all', 'wavefront', 'pipeline', 'parallel_for', 'frame_loop' (default: all)\n"
                      << "  --threads <N>       Number of worker threads (default: hardware concurrency)\n"
                      << "  --iterations <N>    Number of benchmark iterations (default: 10, or 100 for frame_loop)\n"
                      << "  --json <path>       Export structured JSON metrics to specified file path\n"
                      << "  --help, -h          Display this help message and exit\n";
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << "\nRun with --help for available options.\n";
            return 1;
        }
    }

    if (workloadFilter != "all" &&
        workloadFilter != "wavefront" &&
        workloadFilter != "pipeline" &&
        workloadFilter != "parallel_for" &&
        workloadFilter != "frame_loop") {
        std::cerr << "Invalid workload: '" << workloadFilter << "'. Supported: all, wavefront, pipeline, parallel_for, frame_loop\n";
        return 1;
    }

    const std::string platformStr = getPlatformString();
    std::vector<WorkloadResult> executedResults;

    // 1. Wavefront DAG
    if (workloadFilter == "all" || workloadFilter == "wavefront") {
        std::cout << ">>> Running Workload: 50,000-Task Wavefront (Fan-Out & Fan-In DAG)... " << std::flush;
        auto res = benchmarkWavefront(threadCount, iterations);
        std::cout << "Done.\n";
        printAsciiComparisonTable(platformStr, res);
        executedResults.push_back(std::move(res));
    }

    // 2. Multi-Stage Pipeline
    if (workloadFilter == "all" || workloadFilter == "pipeline") {
        std::cout << ">>> Running Workload: 50,000-Task Multi-Stage Pipeline (10x5,000 Tasks)... " << std::flush;
        auto res = benchmarkPipeline(threadCount, iterations);
        std::cout << "Done.\n";
        printAsciiComparisonTable(platformStr, res);
        executedResults.push_back(std::move(res));
    }

    // 3. Parallel For
    if (workloadFilter == "all" || workloadFilter == "parallel_for") {
        std::cout << ">>> Running Workload: 5,000,000-Item Parallel For Workload... " << std::flush;
        auto res = benchmarkParallelFor(threadCount, iterations);
        std::cout << "Done.\n";
        printAsciiComparisonTable(platformStr, res);
        executedResults.push_back(std::move(res));
    }

    // 4. Steady-State Frame Loop
    if (workloadFilter == "all" || workloadFilter == "frame_loop") {
        size_t frames = customIterations ? iterations : 100;
        std::cout << ">>> Running Workload: Steady-State Frame Loop (" << frames << " Frames, 10,000 Tasks/Frame)... " << std::flush;
        auto res = benchmarkFrameLoop(threadCount, frames);
        std::cout << "Done.\n";
        printAsciiComparisonTable(platformStr, res);
        executedResults.push_back(std::move(res));
    }

    // Optional JSON Export
    if (!jsonOutputPath.empty()) {
        writeJsonReport(jsonOutputPath, platformStr, threadCount, executedResults);
    }

    return 0;
}
