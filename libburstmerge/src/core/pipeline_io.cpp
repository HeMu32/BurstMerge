#include "burstmerge/internal/core/pipeline_io.h"

#include "burstmerge/internal/core/pipeline.h"
#include "burstmerge/internal/io/dng_io.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace burstmerge
{
namespace
{

// Local helpers for path and temporary-directory handling.

void Report(const PipelineOrchestrator::ProgressFn& progress,
            float percent,
            const std::string& stage)
{
    if (progress) progress(percent, stage);
}

std::string LowerExt(const std::filesystem::path& p)
{
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c)
    {
        return static_cast<char>(std::tolower(c));
    });
    return ext;
}

bool IsDngPath(const std::string& path)
{
    return LowerExt(std::filesystem::path(path)) == ".dng";
}

std::string GenerateRunId()
{
#ifdef _WIN32
    DWORD pid = GetCurrentProcessId();
    ULONGLONG tick = GetTickCount64();
    char buf[64];
    std::snprintf(buf, sizeof(buf), "run_%lu_%llx", pid, tick);
    return buf;
#else
    auto pid = static_cast<unsigned long>(::getpid());
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return "run_" + std::to_string(pid) + "_" + std::to_string(now);
#endif
}

void OrphanSweep(const std::filesystem::path& parent)
{
    auto max_age = PipelineConstants::kOrphanMaxAge;
    if (!std::filesystem::exists(parent)) return;
    auto now = std::filesystem::file_time_type::clock::now();
    std::error_code ec;
    for (auto& entry : std::filesystem::directory_iterator(parent, ec))
    {
        if (ec)
        { ec.clear(); continue; }
        if (!entry.is_directory()) continue;
        std::string name = entry.path().filename().string();
        if (name.rfind("run_", 0) != 0) continue;
        auto ft = entry.last_write_time(ec);
        if (ec)
        { ec.clear(); continue; }
        auto age = now - ft;
        if (age > max_age)
        {
            std::filesystem::remove_all(entry.path(), ec);
            ec.clear();
        }
    }
}

std::string MakeTempConvertDir(const std::string& output_path)
{
    std::filesystem::path base(output_path);
    std::filesystem::path parent = base.has_parent_path()
        ? base.parent_path() : std::filesystem::current_path();
    std::filesystem::path dir = parent / "burstmerge_converted";
    std::filesystem::create_directories(dir);
    OrphanSweep(dir);
    std::filesystem::path run_dir = dir / GenerateRunId();
    std::filesystem::create_directories(run_dir);
    return run_dir.string();
}

} // namespace

std::vector<std::string> PrepareDngInputs(const std::vector<std::string>& input_paths,
                                          const std::string& output_path,
                                          const PipelineOrchestrator::ProgressFn& progress,
                                          std::string& out_convert_dir)
{
    std::vector<std::string> dng_paths;
    std::vector<std::string> raw_paths;
    dng_paths.reserve(input_paths.size());
    out_convert_dir.clear();

    Report(progress, PipelineConstants::kProgressValidate, "Validating input files");
    for (const auto& path : input_paths)
    {
        if (!std::filesystem::exists(path))
        {
            throw std::runtime_error("Input does not exist: " + path);
        }
        if (IsDngPath(path))
        {
            dng_paths.push_back(path);
        }
        else
        {
            std::string ext = LowerExt(std::filesystem::path(path));
            if (!IsRawExtension(ext))
                throw std::runtime_error("Unsupported file format (not a RAW camera file): " + path);
            raw_paths.push_back(path);
        }
    }

    if (raw_paths.empty()) return dng_paths;

#ifdef _WIN32
    Report(progress, PipelineConstants::kProgressConvertStart, "Preparing RAW to DNG conversion");
    out_convert_dir = MakeTempConvertDir(output_path);
    std::vector<std::string> converted;
    Report(progress,
           PipelineConstants::kProgressConvertStart + 0.02f,
           "Converting " + std::to_string(raw_paths.size()) + " RAW file(s) to DNG");
    if (!RunAdobeDngConverter(raw_paths, out_convert_dir, converted))
    {
        out_convert_dir.clear();
        throw std::runtime_error("Adobe DNG Converter failed or timed out");
    }
    Report(progress, PipelineConstants::kProgressConvertEnd, "RAW to DNG conversion completed");
    dng_paths.insert(dng_paths.end(), converted.begin(), converted.end());
    return dng_paths;
#else
    throw std::runtime_error("Non-DNG RAW input requires pre-conversion on this platform");
#endif
}

} // namespace burstmerge
