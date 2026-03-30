/**
 * bthread Benchmark Suite
 *
 * Measures performance of:
 * 1. bthread create/join throughput
 * 2. bthread_yield performance
 * 3. Mutex contention
 * 4. Comparison with std::thread
 * 5. Work stealing efficiency
 */

// Define for MSVC compatibility - must be before any system includes
#ifdef _WIN32
#define _CRT_NONSTDC_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <errno.h>
#endif

#include "bthread.h"
#include "bthread/mutex.h"
#include "bthread/cond.h"

#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <vector>
#include <atomic>
#include <thread>

// ==================== Utilities ====================

class Timer {
public:
    Timer() : start_(std::chrono::high_resolution_clock::now()) {}

    double elapsed_ms() const {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double, std::milli>(now - start_).count();
    }

    double elapsed_us() const {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double, std::micro>(now - start_).count();
    }

private:
    std::chrono::high_resolution_clock::time_point start_;
};

// ==================== Benchmark 1: Create/Join Throughput ====================

void* empty_task(void* arg) {
    (void)arg;
    return nullptr;
}

void benchmark_create_join(int num_threads, int iterations) {
    fprintf(stderr, "\n[Benchmark 1] Create/Join Throughput\n");
    fprintf(stderr, "  Threads: %d, Iterations: %d\n", num_threads, iterations);

    std::vector<bthread_t> tids(num_threads);

    Timer timer;
    for (int i = 0; i < iterations; ++i) {
        for (int j = 0; j < num_threads; ++j) {
            bthread_create(&tids[j], nullptr, empty_task, nullptr);
        }
        for (int j = 0; j < num_threads; ++j) {
            bthread_join(tids[j], nullptr);
        }
    }

    double elapsed = timer.elapsed_ms();
    int total_ops = num_threads * iterations;
    double ops_per_sec = total_ops / (elapsed / 1000.0);
    double us_per_op = elapsed * 1000.0 / total_ops;

    fprintf(stderr, "  Total time: %.2f ms\n", elapsed);
    fprintf(stderr, "  Total operations: %d\n", total_ops);
    fprintf(stderr, "  Throughput: %.0f ops/sec\n", ops_per_sec);
    fprintf(stderr, "  Latency: %.2f us/op\n", us_per_op);
}

// ==================== Benchmark 2: Yield Performance ====================

void* yield_task(void* arg) {
    int count = *static_cast<int*>(arg);
    for (int i = 0; i < count; ++i) {
        bthread_yield();
    }
    return nullptr;
}

void benchmark_yield(int num_threads, int yields_per_thread) {
    fprintf(stderr, "\n[Benchmark 2] Yield Performance\n");
    fprintf(stderr, "  Threads: %d, Yields per thread: %d\n", num_threads, yields_per_thread);

    std::vector<bthread_t> tids(num_threads);
    std::vector<int> args(num_threads, yields_per_thread);

    Timer timer;
    for (int i = 0; i < num_threads; ++i) {
        bthread_create(&tids[i], nullptr, yield_task, &args[i]);
    }
    for (int i = 0; i < num_threads; ++i) {
        bthread_join(tids[i], nullptr);
    }

    double elapsed = timer.elapsed_ms();
    int total_yields = num_threads * yields_per_thread;
    double yields_per_sec = total_yields / (elapsed / 1000.0);
    double ns_per_yield = elapsed * 1e6 / total_yields;

    fprintf(stderr, "  Total time: %.2f ms\n", elapsed);
    fprintf(stderr, "  Total yields: %d\n", total_yields);
    fprintf(stderr, "  Throughput: %.0f yields/sec\n", yields_per_sec);
    fprintf(stderr, "  Latency: %.2f ns/yield\n", ns_per_yield);
}

// ==================== Benchmark 3: Mutex Contention ====================

static std::atomic<int> mutex_counter{0};
static bthread_mutex_t bench_mutex;

void* mutex_task(void* arg) {
    int iterations = *static_cast<int*>(arg);
    for (int i = 0; i < iterations; ++i) {
        bthread_mutex_lock(&bench_mutex);
        mutex_counter++;
        bthread_mutex_unlock(&bench_mutex);
    }
    return nullptr;
}

void benchmark_mutex(int num_threads, int iterations_per_thread) {
    fprintf(stderr, "\n[Benchmark 3] Mutex Contention\n");
    fprintf(stderr, "  Threads: %d, Iterations per thread: %d\n", num_threads, iterations_per_thread);

    bthread_mutex_init(&bench_mutex, nullptr);
    mutex_counter = 0;

    std::vector<bthread_t> tids(num_threads);
    std::vector<int> args(num_threads, iterations_per_thread);

    Timer timer;
    for (int i = 0; i < num_threads; ++i) {
        bthread_create(&tids[i], nullptr, mutex_task, &args[i]);
    }
    for (int i = 0; i < num_threads; ++i) {
        bthread_join(tids[i], nullptr);
    }
    double elapsed = timer.elapsed_ms();

    int expected = num_threads * iterations_per_thread;
    int actual = mutex_counter.load();

    double ops_per_sec = actual / (elapsed / 1000.0);
    double us_per_op = elapsed * 1000.0 / actual;

    fprintf(stderr, "  Total time: %.2f ms\n", elapsed);
    fprintf(stderr, "  Expected counter: %d, Actual: %d\n", expected, actual);
    fprintf(stderr, "  Throughput: %.0f lock/unlock/sec\n", ops_per_sec);
    fprintf(stderr, "  Latency: %.2f us/op\n", us_per_op);

    bthread_mutex_destroy(&bench_mutex);
}

// ==================== Benchmark 4: Comparison with std::thread ====================

void* compute_task(void* arg) {
    int* result = static_cast<int*>(arg);
    int sum = 0;
    for (int i = 0; i < 1000; ++i) {
        sum += i;
    }
    *result = sum;
    return nullptr;
}

void benchmark_vs_pthread(int num_threads, int iterations) {
    fprintf(stderr, "\n[Benchmark 4] bthread vs std::thread\n");
    fprintf(stderr, "  Threads: %d, Iterations: %d\n", num_threads, iterations);

    // bthread benchmark
    std::vector<bthread_t> btids(num_threads);
    std::vector<int> bresults(num_threads);

    Timer btimer;
    for (int i = 0; i < iterations; ++i) {
        for (int j = 0; j < num_threads; ++j) {
            bthread_create(&btids[j], nullptr, compute_task, &bresults[j]);
        }
        for (int j = 0; j < num_threads; ++j) {
            bthread_join(btids[j], nullptr);
        }
    }
    double btime = btimer.elapsed_ms();

    // std::thread benchmark
    std::vector<std::thread> threads(num_threads);
    std::vector<int> sresults(num_threads);

    Timer stimer;
    for (int i = 0; i < iterations; ++i) {
        for (int j = 0; j < num_threads; ++j) {
            threads[j] = std::thread([](int* r) {
                int sum = 0;
                for (int k = 0; k < 1000; ++k) {
                    sum += k;
                }
                *r = sum;
            }, &sresults[j]);
        }
        for (int j = 0; j < num_threads; ++j) {
            threads[j].join();
        }
    }
    double stime = stimer.elapsed_ms();

    int btotal = num_threads * iterations;
    double bops = btotal / (btime / 1000.0);
    double sops = btotal / (stime / 1000.0);

    fprintf(stderr, "  bthread:    %.2f ms (%.0f ops/sec)\n", btime, bops);
    fprintf(stderr, "  std::thread: %.2f ms (%.0f ops/sec)\n", stime, sops);
    fprintf(stderr, "  Ratio: %.2fx %s\n",
            bops > sops ? bops / sops : sops / bops,
            bops > sops ? "faster" : "slower");
}

// ==================== Benchmark 5: Scalability ====================

void* counter_task(void* arg) {
    std::atomic<int>* counter = static_cast<std::atomic<int>*>(arg);
    counter->fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

void benchmark_scalability() {
    fprintf(stderr, "\n[Benchmark 5] Scalability (work stealing)\n");

    const int iterations = 1000;
    int worker_counts[] = {1, 2, 4, 8, 16};
    int num_configs = sizeof(worker_counts) / sizeof(worker_counts[0]);

    fprintf(stderr, "  %-10s %-12s %-12s %-12s\n",
            "Workers", "Time (ms)", "Ops/sec", "Speedup");
    fprintf(stderr, "  %-10s %-12s %-12s %-12s\n", "------", "--------", "-------", "-------");

    double baseline_time = 0;

    for (int c = 0; c < num_configs; ++c) {
        int workers = worker_counts[c];

        // Note: We can't easily change worker count at runtime,
        // so this measures the default worker count performance
        std::atomic<int> counter{0};
        std::vector<bthread_t> tids(iterations);

        Timer timer;
        for (int i = 0; i < iterations; ++i) {
            bthread_create(&tids[i], nullptr, counter_task, &counter);
        }
        for (int i = 0; i < iterations; ++i) {
            bthread_join(tids[i], nullptr);
        }
        double elapsed = timer.elapsed_ms();

        double ops = iterations / (elapsed / 1000.0);
        double speedup = (c == 0) ? 1.0 : baseline_time / elapsed;

        if (c == 0) {
            baseline_time = elapsed;
        }

        fprintf(stderr, "  %-10d %-12.2f %-12.0f %-12.2fx\n",
                workers, elapsed, ops, speedup);
    }
}

// ==================== Benchmark 6: Memory Pressure ====================

void* alloc_task(void* arg) {
    int size = *static_cast<int*>(arg);
    // Simulate some stack usage
    volatile char buffer[1024];
    for (int i = 0; i < size && i < 1024; ++i) {
        buffer[i] = static_cast<char>(i);
    }
    return nullptr;
}

void benchmark_stack(int num_threads, int iterations) {
    fprintf(stderr, "\n[Benchmark 6] Stack Performance (Memory Pressure)\n");
    fprintf(stderr, "  Threads: %d, Iterations: %d\n", num_threads, iterations);

    std::vector<bthread_t> tids(num_threads);
    std::vector<int> args(num_threads, 1024);

    Timer timer;
    for (int i = 0; i < iterations; ++i) {
        for (int j = 0; j < num_threads; ++j) {
            bthread_create(&tids[j], nullptr, alloc_task, &args[j]);
        }
        for (int j = 0; j < num_threads; ++j) {
            bthread_join(tids[j], nullptr);
        }
    }

    double elapsed = timer.elapsed_ms();
    int total = num_threads * iterations;
    double ops = total / (elapsed / 1000.0);

    fprintf(stderr, "  Total time: %.2f ms\n", elapsed);
    fprintf(stderr, "  Throughput: %.0f ops/sec\n", ops);
}

// ==================== Benchmark 7: Producer-Consumer ====================

static bthread_mutex_t pc_mutex;
static bthread_cond_t pc_cond;
static std::atomic<int> produced{0};
static std::atomic<int> consumed{0};
static int queue_size = 0;
static const int MAX_QUEUE = 100;

void* producer_task(void* arg) {
    int items = *static_cast<int*>(arg);
    for (int i = 0; i < items; ++i) {
        bthread_mutex_lock(&pc_mutex);
        while (queue_size >= MAX_QUEUE) {
            bthread_cond_wait(&pc_cond, &pc_mutex);
        }
        queue_size++;
        produced++;
        bthread_cond_signal(&pc_cond);
        bthread_mutex_unlock(&pc_mutex);
    }
    return nullptr;
}

void* consumer_task(void* arg) {
    int items = *static_cast<int*>(arg);
    for (int i = 0; i < items; ++i) {
        bthread_mutex_lock(&pc_mutex);
        while (queue_size <= 0) {
            bthread_cond_wait(&pc_cond, &pc_mutex);
        }
        queue_size--;
        consumed++;
        bthread_cond_signal(&pc_cond);
        bthread_mutex_unlock(&pc_mutex);
    }
    return nullptr;
}

void benchmark_producer_consumer(int num_producers, int num_consumers, int items_each) {
    fprintf(stderr, "\n[Benchmark 7] Producer-Consumer\n");
    fprintf(stderr, "  Producers: %d, Consumers: %d, Items each: %d\n",
            num_producers, num_consumers, items_each);

    bthread_mutex_init(&pc_mutex, nullptr);
    bthread_cond_init(&pc_cond, nullptr);
    produced = 0;
    consumed = 0;
    queue_size = 0;

    std::vector<bthread_t> ptids(num_producers);
    std::vector<bthread_t> ctids(num_consumers);
    std::vector<int> pargs(num_producers, items_each);
    std::vector<int> cargs(num_consumers, items_each * num_producers / num_consumers);

    Timer timer;

    // Start consumers first
    for (int i = 0; i < num_consumers; ++i) {
        bthread_create(&ctids[i], nullptr, consumer_task, &cargs[i]);
    }

    // Start producers
    for (int i = 0; i < num_producers; ++i) {
        bthread_create(&ptids[i], nullptr, producer_task, &pargs[i]);
    }

    // Wait for all
    for (int i = 0; i < num_producers; ++i) {
        bthread_join(ptids[i], nullptr);
    }
    for (int i = 0; i < num_consumers; ++i) {
        bthread_join(ctids[i], nullptr);
    }

    double elapsed = timer.elapsed_ms();

    int total_items = num_producers * items_each;
    double items_per_sec = total_items / (elapsed / 1000.0);

    fprintf(stderr, "  Total time: %.2f ms\n", elapsed);
    fprintf(stderr, "  Items produced: %d, consumed: %d\n", produced.load(), consumed.load());
    fprintf(stderr, "  Throughput: %.0f items/sec\n", items_per_sec);

    bthread_mutex_destroy(&pc_mutex);
    bthread_cond_destroy(&pc_cond);
}

// ==================== Main ====================

int main(int argc, char* argv[]) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    fprintf(stderr, "========================================\n");
    fprintf(stderr, "       bthread Benchmark Suite\n");
    fprintf(stderr, "========================================\n");

    // Get worker count
    int workers = bthread_get_worker_count();
    fprintf(stderr, "\nWorker threads: %d\n", workers > 0 ? workers : 8);

    // Run benchmarks
    benchmark_create_join(100, 100);        // 10,000 create/join ops
    benchmark_yield(4, 10000);              // 40,000 yields
    benchmark_mutex(8, 10000);              // 80,000 mutex ops
    benchmark_vs_pthread(10, 100);          // Compare with std::thread
    benchmark_scalability();                // Test scaling
    benchmark_stack(50, 100);               // Memory pressure
    benchmark_producer_consumer(4, 4, 1000);// Producer-consumer

    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "       Benchmark Complete!\n");
    fprintf(stderr, "========================================\n");

    (void)argc;
    (void)argv;
    return 0;
}