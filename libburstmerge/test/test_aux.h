#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include "burstmerge/internal/io/image_decoder.h"

namespace fs = std::filesystem;

std::string Lower(std::string s);
std::string ExecCmd(const std::string& cmd);
bool ToolExists(const std::string& name);
std::string CaptureStderr(const std::function<void()>& fn);
int GetExifBitsPerSample(const fs::path& path);
void CreateRgb24Bmp(const fs::path& path, int w, int h);
void CreateMinimalCmykTiff(const fs::path& path);
void TagAs10BitTiff(const fs::path& path);

burstmerge::io::DecodedImage ReadDecodedImage(const fs::path& path);
uint32_t ChannelCount(const burstmerge::io::DecodedImage& img);
float SampleValue(const burstmerge::io::DecodedImage& img,
                  uint32_t x,
                  uint32_t y,
                  uint32_t c);

struct SamplePoint
{
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t c = 0;
    float value = 0.0f;
};

SamplePoint FindSampleNear(const burstmerge::io::DecodedImage& img, float target);
