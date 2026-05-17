#include "burstmerge/api_c.h"
#include "burstmerge/api.h"
#include <string>
#include <cstring>

struct CContext
{
    burstmerge::BurstMerge* bm = nullptr;
    burstmerge::Settings settings;
    std::string last_error;
    BM_ProgressCb progress_cb = nullptr;
    void* progress_user = nullptr;
};

extern "C"
{

#define BM_BACKEND_CPU    0
#define BM_BACKEND_VULKAN 1

BM_Context BM_Create(int backend_type)
{
    auto* ctx = new CContext();
    switch (backend_type)
    {
        case 0:
            ctx->bm = new burstmerge::BurstMerge(burstmerge::BackendType::CPU);
            break;
        default:
            ctx->bm = new burstmerge::BurstMerge(burstmerge::BackendType::Vulkan);
            break;
    }
    return ctx;
}

void BM_Destroy(BM_Context ctx)
{
    auto* c = static_cast<CContext*>(ctx);
    if (c)
    {
        delete c->bm;
        delete c;
    }
}

void BM_AddImage(BM_Context ctx, const char* path)
{
    if (ctx) static_cast<CContext*>(ctx)->bm->AddImage(path);
}

void BM_SetTileSize(BM_Context ctx, int size)
{
    if (ctx) static_cast<CContext*>(ctx)->settings.tile_size = size;
}

void BM_SetSearchDistance(BM_Context ctx, int dist)
{
    if (ctx) static_cast<CContext*>(ctx)->settings.search_distance = dist;
}

void BM_SetNoiseReduction(BM_Context ctx, float strength)
{
    if (ctx) static_cast<CContext*>(ctx)->settings.noise_reduction = strength;
}

void BM_SetExposureMode(BM_Context ctx, int mode)
{
    if (ctx) static_cast<CContext*>(ctx)->settings.exposure_mode =
        static_cast<burstmerge::ExposureMode>(mode);
}

void BM_SetMergeAlgorithm(BM_Context ctx, int algo)
{
    if (ctx) static_cast<CContext*>(ctx)->settings.merge_algo =
        static_cast<burstmerge::MergeAlgorithm>(algo);
}

void BM_SetExposureStops(BM_Context ctx, float stops)
{
    if (ctx)
    {
        static_cast<CContext*>(ctx)->settings.exposure_stops = stops;
    }
}

void BM_SetProgressCallback(BM_Context ctx, BM_ProgressCb cb, void* user)
{
    if (ctx)
    {
        static_cast<CContext*>(ctx)->progress_cb = cb;
        static_cast<CContext*>(ctx)->progress_user = user;
    }
}

int BM_Process(BM_Context ctx, const char* out_dir)
{
    if (!ctx) return 0;
    auto* c = static_cast<CContext*>(ctx);

    // Wire up the latest progress callback
    c->bm->SetProgressCallback([c](float p, const std::string& s)
    {
        if (c->progress_cb)
            c->progress_cb(p, s.c_str(), c->progress_user);
    });

    c->bm->Configure(c->settings);
    auto result = c->bm->Process(out_dir);
    if (!result.success)
    {
        c->last_error = result.error_msg;
    }
    return result.success ? 1 : 0;
}

const char* BM_GetLastError(BM_Context ctx)
{
    if (!ctx) return "";
    return static_cast<CContext*>(ctx)->last_error.c_str();
}

} // extern "C"
