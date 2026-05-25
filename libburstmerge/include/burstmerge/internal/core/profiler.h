#pragma once

#include <cstdint>
#include <string>

namespace burstmerge
{

bool ProfileEnabled();
void ResetProfiler();

// Time stats use the entry name as a scope identifier. Each AddProfileTime()
// call increments the entry count by 1 and accumulates nanoseconds into the
// same row, so the report shows call count, total time and average time.
void AddProfileTime(const char* name, uint64_t nanoseconds);

// Counter stats use the entry name as a logical event identifier. The value is
// added directly to the counter row and has no time meaning.
//
// Important profiler naming conventions used by the task executor:
// - counter.parallel_for.submitted.<tag>
//   Counts how many times the tagged task function actually ran. For a
//   parallel loop this is the number of submitted chunks; for a serial
//   fallback this is 1.
// - counter.parallel_for.tasks.<tag>
//   Accumulates the worker budget assigned to that tagged task across all
//   parallel invocations. Each parallel call contributes its per-call worker
//   limit; serial fallbacks do not increment this counter.
void AddProfileCounter(const char* name, uint64_t value = 1);
std::string BuildProfileReport();

class ProfileScope
{
public:
    explicit ProfileScope(const char* name);
    ~ProfileScope();

private:
    const char* name_;
    uint64_t start_ns_ = 0;
    bool enabled_ = false;
};

} // namespace burstmerge
