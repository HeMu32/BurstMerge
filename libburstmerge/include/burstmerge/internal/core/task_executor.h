#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace burstmerge
{

// Shared internal CPU executor used by data-parallel loops.  It intentionally
// exposes range-based helpers instead of raw futures so algorithms can keep a
// deterministic "each worker writes a disjoint output region" structure.
void ParallelFor(size_t begin,
                 size_t end,
                 size_t grain,
                 const std::function<void(size_t, size_t)>& fn);

inline void ParallelFor(size_t end,
                        size_t grain,
                        const std::function<void(size_t, size_t)>& fn)
{
    ParallelFor(0, end, grain, fn);
}

void ParallelForRows(uint32_t begin_row,
                     uint32_t end_row,
                     uint32_t grain_rows,
                     const std::function<void(uint32_t, uint32_t)>& fn);

bool CanRunInParallel();

inline void ParallelForRows(uint32_t rows,
                            uint32_t grain_rows,
                            const std::function<void(uint32_t, uint32_t)>& fn)
{
    ParallelForRows(0, rows, grain_rows, fn);
}

} // namespace burstmerge
