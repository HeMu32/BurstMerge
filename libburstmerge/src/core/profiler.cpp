#include "burstmerge/internal/core/profiler.h"

#ifndef NDEBUG
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>
#endif

namespace burstmerge
{
#ifndef NDEBUG
namespace
{

using Clock = std::chrono::steady_clock;

struct ProfileStat
{
    // For time rows this is the number of timed scope completions.
    // For counter rows this is the accumulated counter value.
    uint64_t count = 0;

    // Only used by time rows. Counter rows leave this at 0.
    uint64_t total_ns = 0;
};

std::mutex& ProfilerMutex()
{
    static std::mutex m;
    return m;
}

std::unordered_map<std::string, ProfileStat>& ProfilerMap()
{
    static std::unordered_map<std::string, ProfileStat> s;
    return s;
}

uint64_t NowNs()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}

} // namespace
#endif

bool ProfileEnabled()
{
#ifdef NDEBUG
    return false;
#else
    static int enabled = []()
    {
        const char* env = std::getenv("BURSTMERGE_PROFILE");
        return (env && env[0] && env[0] != '0') ? 1 : 0;
    }();
    return enabled != 0;
#endif
}

void ResetProfiler()
{
#ifdef NDEBUG
    return;
#else
    if (!ProfileEnabled()) return;
    std::lock_guard<std::mutex> lock(ProfilerMutex());
    ProfilerMap().clear();
#endif
}

void AddProfileTime(const char* name, uint64_t nanoseconds)
{
#ifdef NDEBUG
    (void)name;
    (void)nanoseconds;
    return;
#else
    if (!ProfileEnabled()) return;
    std::lock_guard<std::mutex> lock(ProfilerMutex());
    auto& s = ProfilerMap()[name];
    s.count += 1;
    s.total_ns += nanoseconds;
#endif
}

void AddProfileCounter(const char* name, uint64_t value)
{
#ifdef NDEBUG
    (void)name;
    (void)value;
    return;
#else
    if (!ProfileEnabled()) return;
    std::lock_guard<std::mutex> lock(ProfilerMutex());
    auto& s = ProfilerMap()[name];
    s.count += value;
#endif
}

std::string BuildProfileReport()
{
#ifdef NDEBUG
    return std::string();
#else
    if (!ProfileEnabled()) return std::string();

    std::vector<std::pair<std::string, ProfileStat>> rows;
    {
        std::lock_guard<std::mutex> lock(ProfilerMutex());
        rows.assign(ProfilerMap().begin(), ProfilerMap().end());
    }
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b)
    {
        return a.first < b.first;
    });

    std::ostringstream oss;
    oss << "[PROFILE] ---- begin ----\n";
    for (const auto& row : rows)
    {
        const auto& name = row.first;
        const auto& stat = row.second;
        if (name.rfind("counter.", 0) == 0)
        {
            // Counter rows are reported as a single accumulated value. Their
            // semantics are defined by the counter name at the call site.
            oss << "[PROFILE] " << name << " = " << stat.count << "\n";
        }
        else
        {
            // Time rows report how many times the scope completed, plus total
            // and average wall time across all recorded completions.
            double total_ms = static_cast<double>(stat.total_ns) / 1.0e6;
            double avg_ms = stat.count ? total_ms / static_cast<double>(stat.count) : 0.0;
            oss << "[PROFILE] " << name
                << " count=" << stat.count
                << " total_ms=" << total_ms
                << " avg_ms=" << avg_ms << "\n";
        }
    }
    oss << "[PROFILE] ---- end ----\n";
    return oss.str();
#endif
}

ProfileScope::ProfileScope(const char* name)
    : name_(name)
{
#ifndef NDEBUG
    enabled_ = ProfileEnabled();
    if (enabled_) start_ns_ = NowNs();
#endif
}

ProfileScope::~ProfileScope()
{
#ifdef NDEBUG
    return;
#else
    if (!enabled_) return;
    AddProfileTime(name_, NowNs() - start_ns_);
#endif
}

} // namespace burstmerge
