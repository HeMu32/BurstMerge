#include "burstmerge/internal/core/task_executor.h"

#include "burstmerge/internal/core/profiler.h"

#include <algorithm>
#include <cstdlib>

#include <omp.h>

namespace burstmerge
{
namespace
{

int RequestedThreadCount()
{
    if (const char* env = std::getenv("BURSTMERGE_THREADS"))
    {
        int parsed = std::atoi(env);
        if (parsed >= 1) return parsed;
    }
    return std::max(1, omp_get_max_threads());
}

size_t GrainScale()
{
    if (const char* env = std::getenv("BURSTMERGE_GRAIN_SCALE"))
    {
        int parsed = std::atoi(env);
        if (parsed >= 1) return static_cast<size_t>(parsed);
    }
    return 1;
}

} // namespace

size_t ParallelismHint()
{
    return static_cast<size_t>(std::max(1, RequestedThreadCount()));
}

uint32_t RecommendedImageRowGrain(uint32_t width,
                                  uint32_t channels,
                                  uint32_t min_pixels,
                                  uint32_t min_rows)
{
    const uint64_t denom = std::max<uint64_t>(1,
        static_cast<uint64_t>(width) * std::max<uint32_t>(1, channels));
    return static_cast<uint32_t>(std::max<uint64_t>(min_rows,
        (static_cast<uint64_t>(min_pixels) + denom - 1) / denom));
}

uint32_t RecommendedBandCount(uint32_t items,
                              uint32_t bands_per_thread,
                              uint32_t max_bands)
{
    if (items == 0) return 0;
    const uint32_t threads = static_cast<uint32_t>(ParallelismHint());
    const uint32_t target = std::max<uint32_t>(1, threads * std::max<uint32_t>(1, bands_per_thread));
    return std::min(items, std::max<uint32_t>(1, std::min(target, max_bands)));
}

void ParallelFor(size_t begin,
                 size_t end,
                 size_t grain,
                 const std::function<void(size_t, size_t)>& fn)
{
    ProfileScope scope("time.parallel_for.total");
    AddProfileCounter("counter.parallel_for.calls");
    if (end <= begin)
    {
        return;
    }

    const size_t total = end - begin;
    if (grain == 0)
    {
        grain = total;
    }

    const int thread_count = RequestedThreadCount();
    if (thread_count <= 1 || omp_in_parallel() || total <= grain)
    {
        AddProfileCounter("counter.parallel_for.serial_fallback");
        fn(begin, end);
        return;
    }

    grain *= GrainScale();
    if (grain == 0)
    {
        grain = total;
    }

    const size_t max_task_count = (total + grain - 1) / grain;
    const size_t task_count = std::min<size_t>(max_task_count, static_cast<size_t>(thread_count));
    const size_t chunk_size = (total + task_count - 1) / task_count;
    AddProfileCounter("counter.parallel_for.parallel_calls");
    AddProfileCounter("counter.parallel_for.tasks", static_cast<uint64_t>(task_count));

#pragma omp parallel for schedule(static) num_threads(thread_count)
    for (int task_idx = 0; task_idx < static_cast<int>(task_count); ++task_idx)
    {
        const size_t chunk_begin = begin + static_cast<size_t>(task_idx) * chunk_size;
        const size_t chunk_end = std::min(end, chunk_begin + chunk_size);
        if (chunk_begin < chunk_end)
        {
            fn(chunk_begin, chunk_end);
        }
    }
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
