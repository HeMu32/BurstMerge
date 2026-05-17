#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

#if defined(BM_BUILD_SHARED)
  #if defined(_WIN32)
    #define BM_API __declspec(dllexport)
  #else
    #define BM_API __attribute__((visibility("default")))
  #endif
#else
  #define BM_API
#endif

#define BM_BACKEND_CPU    0
#define BM_BACKEND_VULKAN 1

#define BM_ALGO_SPATIAL   0
#define BM_ALGO_FREQUENCY 1

#define BM_EXPOSURE_OFF   0
#define BM_EXPOSURE_LINEAR 1
#define BM_EXPOSURE_CURVE 2

typedef void* BM_Context;
typedef void (*BM_ProgressCb)(float percent, const char* stage, void* user);

BM_API BM_Context  BM_Create(int backend_type);
BM_API void        BM_Destroy(BM_Context ctx);
BM_API void        BM_AddImage(BM_Context ctx, const char* path);
BM_API void        BM_SetTileSize(BM_Context ctx, int size);
BM_API void        BM_SetSearchDistance(BM_Context ctx, int dist);
BM_API void        BM_SetNoiseReduction(BM_Context ctx, float strength);
BM_API void        BM_SetExposureMode(BM_Context ctx, int mode);
BM_API void        BM_SetMergeAlgorithm(BM_Context ctx, int algo);
BM_API void        BM_SetExposureStops(BM_Context ctx, float stops);
BM_API void        BM_SetProgressCallback(BM_Context ctx, BM_ProgressCb cb, void* user);
BM_API int         BM_Process(BM_Context ctx, const char* out_dir);
BM_API const char* BM_GetLastError(BM_Context ctx);

#ifdef __cplusplus
}
#endif
