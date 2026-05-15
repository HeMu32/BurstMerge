#pragma once
#include "burstmerge/internal/core/image_buffer.h"
#include "burstmerge/internal/core/sub_graph.h"
#include "burstmerge/api.h"

namespace burstmerge {

class IComputeBackend {
public:
    virtual ~IComputeBackend() = default;

    virtual bool Initialize() = 0;
    virtual void Shutdown() = 0;
    virtual void Synchronize() = 0;

    virtual DeviceBuffer CreateDeviceTexture(uint32_t w, uint32_t h, uint32_t d,
                                              PixelFormat fmt) = 0;
    virtual DeviceBuffer CreateStaging(uint32_t w, uint32_t h, PixelFormat fmt) = 0;
    virtual void DestroyBuffer(DeviceBuffer& buf) = 0;
    virtual void Upload(const HostBuffer& src, DeviceBuffer& dst) = 0;
    virtual void Download(const DeviceBuffer& src, HostBuffer& dst) = 0;
    virtual void Copy(const DeviceBuffer& src, DeviceBuffer& dst) = 0;

    virtual void ExecuteSubGraph(const SubGraph& graph,
                                 const Settings& settings) = 0;
};

} // namespace burstmerge
