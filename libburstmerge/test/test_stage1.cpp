#include "burstmerge/api.h"
#include "burstmerge/internal/io/dng_io.h"

#include <cstdio>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

static int g_checks = 0;
static int g_failed = 0;

#define CHECK(cond, msg) do { \
    ++g_checks; \
    if (!(cond)) { \
        std::cerr << "  FAIL [" << __LINE__ << "]: " << msg << std::endl; \
        ++g_failed; \
    } \
} while (0)

bool FileExists(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

long FileSize(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return -1;
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fclose(f);
    return size;
}

std::vector<std::string> FilesWithExt(const fs::path& dir, const std::string& ext) {
    std::vector<std::string> files;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        std::string e = entry.path().extension().string();
        for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (e == ext) files.push_back(entry.path().string());
    }
    std::sort(files.begin(), files.end());
    return files;
}

bool ConverterAvailable() {
#ifdef _WIN32
    DWORD attr = GetFileAttributesA("C:\\Program Files\\Adobe\\Adobe DNG Converter\\Adobe DNG Converter.exe");
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    return false;
#endif
}

void ProcessAndVerify(const std::string& name,
                      const std::vector<std::string>& inputs,
                      const std::string& output_path)
{
    std::cout << "[test] process " << name << "..." << std::endl;
    CHECK(!inputs.empty(), name + " inputs non-empty");
    if (inputs.empty()) return;

    fs::create_directories(fs::path(output_path).parent_path());
    std::remove(output_path.c_str());

    burstmerge::BurstMerge bm(burstmerge::BackendType::CPU);
    burstmerge::Settings settings;
    settings.merge_algo = burstmerge::MergeAlgorithm::Spatial;
    settings.noise_reduction = 13.0f;
    bm.Configure(settings);

    int progress_calls = 0;
    float last_progress = -1.0f;
    bm.SetProgressCallback([&](float p, const std::string& stage) {
        (void)stage;
        ++progress_calls;
        last_progress = p;
    });

    for (const auto& input : inputs) bm.AddImage(input);
    auto result = bm.Process(output_path);

    if (!result.success) {
        std::cout << "  error: " << result.error_msg << std::endl;
    }
    CHECK(result.success, name + " process succeeds");
    CHECK(result.output_path == output_path, name + " output path exact");
    CHECK(FileExists(output_path), name + " output exists");
    CHECK(FileSize(output_path) > 1024, name + " output size > 1KB");
    CHECK(progress_calls >= 2, name + " progress callbacks");
    CHECK(last_progress == 1.0f, name + " progress reaches 1");

    if (FileExists(output_path)) {
        burstmerge::DngReader reader(output_path.c_str());
        auto image = reader.Read();
        CHECK(image.metadata.width > 0, name + " readable output width");
        CHECK(image.metadata.height > 0, name + " readable output height");
        CHECK(image.pixels.data != nullptr, name + " readable output pixels");
    }
}

int main() {
    fs::path root(TEST_DATA_DIR);
    fs::path build(TEST_BINARY_DIR);
    fs::path samples = root / "libburstmerge" / "test" / "samples";
    fs::path out_dir = build / "stage1_outputs";

    ProcessAndVerify("single_dng",
        { (samples / "X1M5_Wide.dng").string() },
        (out_dir / "single_dng_output.dng").string());

    if (ConverterAvailable()) {
        auto seq = FilesWithExt(samples / "Seq", ".arw");
        auto bkt = FilesWithExt(samples / "Bkt", ".arw");
        ProcessAndVerify("seq_arw_5", seq, (out_dir / "seq_output.dng").string());
        ProcessAndVerify("bkt_arw_5", bkt, (out_dir / "bkt_output.dng").string());
    } else {
        std::cout << "[test] SKIP ARW process tests: Adobe DNG Converter not installed" << std::endl;
    }

    std::cout << "\nStage 1: " << g_checks << " checks, " << g_failed << " failed" << std::endl;
    return g_failed > 0 ? 1 : 0;
}
