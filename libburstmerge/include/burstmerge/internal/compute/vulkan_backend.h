#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace burstmerge
{
namespace vulkan
{

// Push-constant block mirrored from shaders/common.glsl. Order/offsets MUST
// match the GLSL layout exactly (16 ints followed by 8 floats, no padding).
struct ShaderPC
{
    int32_t w;
    int32_t h;
    int32_t channels;
    int32_t w2;
    int32_t h2;
    int32_t channels2;
    int32_t i0;
    int32_t i1;
    int32_t i2;
    int32_t i3;
    int32_t i4;
    int32_t i5;
    int32_t i6;
    int32_t i7;
    int32_t i8;
    int32_t i9;
    float f0;
    float f1;
    float f2;
    float f3;
    float f4;
    float f5;
    float f6;
    float f7;
};

// One bound storage or uniform buffer for a dispatch.
struct Binding
{
    uint32_t binding;        // descriptor binding index (0..7 storage, 8 uniform)
    uint64_t handle;         // buffer handle returned by CreateBuffer / CreateUbo
    uint32_t range;          // bytes used (0 = whole buffer)
};

// Result of Initialize; human-readable for logging.
struct BackendInfo
{
    std::string device_name;
    uint32_t api_version = 0;
    bool valid = false;
};

class VulkanBackend
{
public:
    VulkanBackend();
    ~VulkanBackend();

    VulkanBackend(const VulkanBackend&) = delete;
    VulkanBackend& operator=(const VulkanBackend&) = delete;

    // Creates instance / physical-device / logical-device / queue. Returns
    // false and fills last_error_ on failure (e.g. no Vulkan ICD).
    bool Initialize();
    // Initialize with a specific physical device (-1 = auto-select best).
    bool Initialize(int device_index);
    // Enumerate available compute-capable Vulkan devices by name, without
    // creating a logical device. Index in the returned vector corresponds
    // to device_index passed to Initialize().
    static std::vector<std::string> EnumerateDevices();
    void Shutdown();
    bool IsInitialized() const;

    const BackendInfo& Info() const;
    const std::string& LastError() const;

    // Returns true if the GPU supports shaderFloat64 AND the library was
    // compiled with BURSTMERGE_GPU_FP64=ON. When false, dense alignment uses
    // the float-accumulation shader (less consistent in extreme bracketing).
    bool HasFloat64() const;

    // ---- Buffer management ----
    // Allocate a device-local storage buffer sized for `float_count` floats.
    uint64_t CreateBuffer(uint32_t float_count);
    // Allocate a host-visible buffer (CPU-readable, used for reduce results /
    // staging). Returns handle.
    uint64_t CreateHostBuffer(uint32_t float_count);
    // Allocate a device-local buffer and initialize it with host data
    // (synchronous staging upload).
    uint64_t CreateBufferFromFloats(const float* data, uint32_t float_count);
    // Upload raw uint16 data to a device-local buffer (no CPU-side float
    // conversion). Used by prepare_texture which reads uint16 on GPU.
    uint64_t CreateBufferFromU16(const uint16_t* data, uint32_t count);
    // Allocate a uniform buffer initialized with `bytes` of data.
    uint64_t CreateUbo(const void* data, uint32_t bytes);
    // Replace contents of an existing uniform buffer (created via CreateUbo).
    void UpdateUbo(uint64_t handle, const void* data, uint32_t bytes);
    // Overwrite a device buffer with host data (synchronous staging).
    void UploadFloats(uint64_t handle, const float* data, uint32_t float_count);
    // Download float contents to host (synchronous).
    void DownloadFloats(uint64_t handle, float* out, uint32_t float_count);
    // Download uint32 contents to host (synchronous).
    void DownloadUints(uint64_t handle, uint32_t* out, uint32_t uint_count);
    // Fill an entire buffer with a constant float value.
    void FillFloat(uint64_t handle, float value);
    // Copy float_count floats from src to dst+dst_offset_floats (recorded into current frame).
    void CopyBufferRegion(uint64_t dst, uint32_t dst_offset_floats, uint64_t src, uint32_t float_count);
    // Upload host data to a device buffer, recording the copy into the current
    // frame (no one-shot submit). Staging buffer is freed at next FlushFrame.
    void RecordUpload(uint64_t dst, const void* data, uint32_t byte_count);
    // Number of floats the buffer was created to hold.
    uint32_t BufferFloatCount(uint64_t handle) const;
    void DestroyBuffer(uint64_t handle);
    // Queue a buffer for destruction at the next FlushFrame(). Safe to call
    // between BeginFrame/FlushFrame — the actual vkDestroyBuffer happens
    // after the GPU finishes the current frame. Does NOT add a sync point.
    void DeferredDestroy(uint64_t handle);

    // ---- Compute dispatch ----
    // Recording model: BeginFrame() starts a command buffer; Dispatch() records
    // compute work with an interleaved compute->compute barrier; FlushFrame()
    // submits and waits. Sync points (reduce readbacks) call FlushFrame() first.
    void BeginFrame();
    void FlushFrame();
    // `shader` is the base filename without extension (e.g. "spatial_acc_multi").
    void Dispatch(const char* shader, const ShaderPC& pc,
                  uint32_t gx, uint32_t gy, uint32_t gz,
                  const Binding* bindings, uint32_t num_bindings);

    // Hard wait on the compute queue.
    void Synchronize();

    // After a VulkanBackend is destroyed, these hold the VRAM leak
    // statistics from the most recent instance. Used by tests to verify
    // that all GPU buffers were properly freed.
    static uint64_t LastLeakedBytes();
    static uint64_t LastLeakedBuffers();

    struct Impl;  // forward-declared publicly so the .cpp helpers can name it
private:
    Impl* impl_ = nullptr;
};

} // namespace vulkan
} // namespace burstmerge
