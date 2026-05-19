#include "burstmerge/internal/core/task_executor.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace burstmerge
{
namespace
{

thread_local bool g_in_worker_thread = false;

class TaskExecutor
{
public:
    TaskExecutor()
    {
        unsigned n = 0;
        bool env_override = false;
        if (const char* env = std::getenv("BURSTMERGE_THREADS"))
        {
            int parsed = std::atoi(env);
            if (parsed >= 1)
            {
                env_override = true;
                // Keep one caller thread outside the pool.  BURSTMERGE_THREADS=1
                // means fully serial execution.
                n = static_cast<unsigned>(std::max(0, parsed - 1));
            }
        }
        if (!env_override)
        {
            const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
            n = hw > 1 ? hw - 1 : 0;
        }
        workers_.reserve(n);
        for (unsigned i = 0; i < n; ++i)
        {
            workers_.emplace_back([this]() { WorkerLoop(); });
        }
    }

    ~TaskExecutor()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& worker : workers_)
        {
            if (worker.joinable()) worker.join();
        }
    }

    size_t WorkerCount() const
    {
        return workers_.size();
    }

    void RunTasks(size_t task_count, const std::function<void(size_t)>& fn)
    {
        if (task_count == 0) return;

        std::mutex wait_mutex;
        std::condition_variable wait_cv;
        std::atomic<size_t> remaining(task_count);

        for (size_t i = 0; i < task_count; ++i)
        {
            Enqueue([&, i]() {
                fn(i);
                if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
                {
                    std::lock_guard<std::mutex> lock(wait_mutex);
                    wait_cv.notify_one();
                }
            });
        }

        std::unique_lock<std::mutex> lock(wait_mutex);
        wait_cv.wait(lock, [&]() { return remaining.load(std::memory_order_acquire) == 0; });
    }

private:
    void Enqueue(std::function<void()> task)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(std::move(task));
        }
        cv_.notify_one();
    }

    void WorkerLoop()
    {
        g_in_worker_thread = true;
        for (;;)
        {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [&]() { return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty()) break;
                task = std::move(queue_.front());
                queue_.pop_front();
            }
            task();
        }
        g_in_worker_thread = false;
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
    std::vector<std::thread> workers_;
    bool stop_ = false;
};

TaskExecutor& GetExecutor()
{
    static TaskExecutor executor;
    return executor;
}

} // namespace

void ParallelFor(size_t begin,
                 size_t end,
                 size_t grain,
                 const std::function<void(size_t, size_t)>& fn)
{
    if (end <= begin) return;
    const size_t total = end - begin;
    if (grain == 0) grain = total;

    TaskExecutor& executor = GetExecutor();
    const size_t workers = executor.WorkerCount();
    if (workers == 0 || total <= grain || g_in_worker_thread)
    {
        fn(begin, end);
        return;
    }

    const size_t task_count = (total + grain - 1) / grain;
    executor.RunTasks(task_count, [&](size_t task_idx)
    {
        const size_t chunk_begin = begin + task_idx * grain;
        const size_t chunk_end = std::min(end, chunk_begin + grain);
        if (chunk_begin < chunk_end) fn(chunk_begin, chunk_end);
    });
}

void ParallelForRows(uint32_t begin_row,
                     uint32_t end_row,
                     uint32_t grain_rows,
                     const std::function<void(uint32_t, uint32_t)>& fn)
{
    ParallelFor(static_cast<size_t>(begin_row),
                static_cast<size_t>(end_row),
                static_cast<size_t>(std::max<uint32_t>(1, grain_rows)),
                [&](size_t y0, size_t y1)
                {
                    fn(static_cast<uint32_t>(y0), static_cast<uint32_t>(y1));
                });
}

} // namespace burstmerge
