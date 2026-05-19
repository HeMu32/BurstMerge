#pragma once

#include <cstdint>
#include <string>

namespace burstmerge
{

bool ProfileEnabled();
void ResetProfiler();
void AddProfileTime(const char* name, uint64_t nanoseconds);
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
