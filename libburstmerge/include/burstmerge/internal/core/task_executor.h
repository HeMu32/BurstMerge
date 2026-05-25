#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace burstmerge
{

// ---- Grain-size defaults for row-based parallel loops ----
// Each chunk targets at least kRowGrainMinPixels float-pixel operations,
// which empirically keeps scheduling overhead below ~1 % on modern CPUs.
constexpr uint32_t kRowGrainMinPixels    = 1u << 18;    // 262144 floats, a magic number for CPU processing
constexpr uint32_t kRowGrainMinRows      = 16;          // fallback min rows
constexpr uint32_t kRowGrainCoarseRows   = 32;          // coarser grain for plane images

// ---- Band-count defaults for tile-based parallel loops ----
// These avoid the "millions of tiny tasks" problem by bounding the total
// number of parallel bands to O(threads).
constexpr uint32_t kBandCountPerThread   = 2;           // bands per worker
constexpr uint32_t kBandCountMax         = 64;          // absolute band cap
constexpr uint32_t kBandCountDenseMax    = 32;          // tighter cap for dense/freq alignment

// Shared internal CPU executor used by data-parallel loops.  It intentionally
// exposes range-based helpers instead of raw futures so algorithms can keep a
// deterministic "each worker writes a disjoint output region" structure.
void ParallelFor(size_t begin,
                 size_t end,
                 size_t grain,
                 const std::function<void(size_t, size_t)>& fn,
                 const char* tag = nullptr);

inline void ParallelFor(size_t end,
                        size_t grain,
                        const std::function<void(size_t, size_t)>& fn,
                        const char* tag = nullptr)
{
    ParallelFor(0, end, grain, fn, tag);
}

void ParallelForRows(uint32_t begin_row,
                     uint32_t end_row,
                     uint32_t grain_rows,
                     const std::function<void(uint32_t, uint32_t)>& fn,
                     const char* tag = nullptr);

size_t ParallelismHint();
uint32_t RecommendedImageRowGrain(uint32_t width,
                                  uint32_t channels,
                                  uint32_t min_pixels = kRowGrainMinPixels,
                                  uint32_t min_rows = kRowGrainMinRows);
uint32_t RecommendedBandCount(uint32_t items,
                              uint32_t bands_per_thread = kBandCountPerThread,
                              uint32_t max_bands = kBandCountMax);

inline void ParallelForRows(uint32_t rows,
                            uint32_t grain_rows,
                            const std::function<void(uint32_t, uint32_t)>& fn,
                            const char* tag = nullptr)
{
    ParallelForRows(0, rows, grain_rows, fn, tag);
}

} // namespace burstmerge
