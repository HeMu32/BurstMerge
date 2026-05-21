#include "burstmerge/internal/core/profiler.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace burstmerge
{
namespace
{

using Clock = std::chrono::steady_clock;

struct ProfileStat
{
    uint64_t count = 0;
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

bool ProfileEnabled()
{
    static int enabled = []()
    {
        const char* env = std::getenv("BURSTMERGE_PROFILE");
        return (env && env[0] && env[0] != '0') ? 1 : 0;
    }();
    return enabled != 0;
}

void ResetProfiler()
{
    if (!ProfileEnabled()) return;
    std::lock_guard<std::mutex> lock(ProfilerMutex());
    ProfilerMap().clear();
}

void AddProfileTime(const char* name, uint64_t nanoseconds)
{
    if (!ProfileEnabled()) return;
    std::lock_guard<std::mutex> lock(ProfilerMutex());
    auto& s = ProfilerMap()[name];
    s.count += 1;
    s.total_ns += nanoseconds;
}

void AddProfileCounter(const char* name, uint64_t value)
{
    if (!ProfileEnabled()) return;
    std::lock_guard<std::mutex> lock(ProfilerMutex());
    auto& s = ProfilerMap()[name];
    s.count += value;
}

std::string BuildProfileReport()
{
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
            oss << "[PROFILE] " << name << " = " << stat.count << "\n";
        }
        else
        {
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
}

ProfileScope::ProfileScope(const char* name)
    : name_(name), enabled_(ProfileEnabled())
{
    if (enabled_) start_ns_ = NowNs();
}

ProfileScope::~ProfileScope()
{
    if (!enabled_) return;
    AddProfileTime(name_, NowNs() - start_ns_);
}

} // namespace burstmerge
