#include "burstmerge/internal/compute/vulkan_backend.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

#include "spirv_embedded.inl"

namespace burstmerge
{
namespace vulkan
{

namespace
{
constexpr uint32_t kPushConstantBytes = sizeof(ShaderPC);
constexpr uint32_t kMaxStorageBindings = 8;   // bindings 0..7
constexpr uint32_t kUboBinding = 8;
constexpr uint32_t kMaxFrameDispatches = 8192;

bool FindMemoryType(VkPhysicalDeviceMemoryProperties const& props,
                    uint32_t type_bits,
                    VkMemoryPropertyFlags required,
                    uint32_t& out)
{
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
    {
        if ((type_bits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & required) == required)
        {
            out = i;
            return true;
        }
    }
    return false;
}
} // namespace

struct VulkanBackend::Impl
{
    bool initialized = false;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties mem_props;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    VkCommandPool cmd_pool = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool = VK_NULL_HANDLE;

    VkCommandBuffer frame_cb = VK_NULL_HANDLE;
    bool recording = false;
    uint32_t frame_dispatches = 0;

    std::string last_error;
    BackendInfo info;

    struct Buffer
    {
        VkBuffer buf = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
        uint32_t float_count = 0;
        bool host_visible = false;
        bool ubo = false;
        void* mapped = nullptr;
    };
    std::unordered_map<uint64_t, Buffer> buffers;
    uint64_t next_handle = 1;

    // Deferred destruction queue: DeferredDestroy() appends handles here;
    // FlushFrame() drains the queue after vkQueueWaitIdle, so the GPU is
    // guaranteed idle when the actual vkDestroyBuffer runs. This lets
    // callers queue frees between BeginFrame/FlushFrame pairs without
    // inserting extra sync points.
    std::vector<uint64_t> pending_deferred;

    // VRAM tracking: current live bytes and peak (high-water mark).
    // Updated in alloc_buffer and all destroy paths.
    VkDeviceSize vram_live = 0;
    VkDeviceSize vram_peak = 0;
    uint64_t vram_live_count = 0;  // number of live buffers
    uint64_t vram_peak_count = 0;

    struct StagingBuffer
    {
        VkBuffer buf = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkDeviceSize capacity = 0;
        void* mapped = nullptr;
    };
    StagingBuffer download_staging;

    std::unordered_map<std::string, VkPipeline> pipelines;
    std::vector<std::string> created_pipelines; // for destruction order

    VkCommandBuffer one_shot_begin()
    {
        VkCommandBufferAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = cmd_pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(device, &ai, &cb);
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cb, &bi);
        return cb;
    }
    void one_shot_end_wait(VkCommandBuffer cb)
    {
        vkEndCommandBuffer(cb);
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue);
        vkFreeCommandBuffers(device, cmd_pool, 1, &cb);
    }

    VkPipeline get_pipeline(const char* shader)
    {
        auto it = pipelines.find(shader);
        if (it != pipelines.end()) return it->second;

        SpirvModule mod{};
        if (!GetBurstSpirvByName(shader, mod) || mod.words == 0)
        {
            last_error = std::string("Unknown shader: ") + shader;
            std::fprintf(stderr, "[Vulkan] pipeline FAILED (unknown shader): %s\n", shader);
            return VK_NULL_HANDLE;
        }
        VkShaderModuleCreateInfo mi{};
        mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        mi.codeSize = mod.words * sizeof(uint32_t);
        mi.pCode = mod.data;
        VkShaderModule module_ = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device, &mi, nullptr, &module_) != VK_SUCCESS)
        {
            last_error = std::string("vkCreateShaderModule failed: ") + shader;
            std::fprintf(stderr, "[Vulkan] pipeline FAILED (shader module): %s\n", shader);
            return VK_NULL_HANDLE;
        }
        VkComputePipelineCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = module_;
        ci.stage.pName = "main";
        ci.layout = pipeline_layout;
        VkPipeline pipe = VK_NULL_HANDLE;
        VkResult r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipe);
        vkDestroyShaderModule(device, module_, nullptr);
        if (r != VK_SUCCESS)
        {
            last_error = std::string("vkCreateComputePipelines failed (") + shader + ")";
            std::fprintf(stderr, "[Vulkan] pipeline FAILED (compute pipeline, result=%d): %s\n", r, shader);
            return VK_NULL_HANDLE;
        }
        pipelines[shader] = pipe;
        created_pipelines.push_back(shader);
        return pipe;
    }

    VkDescriptorBufferInfo make_info(const Buffer& b, uint32_t range_bytes)
    {
        VkDescriptorBufferInfo info{};
        info.buffer = b.buf;
        info.offset = 0;
        info.range = (range_bytes == 0) ? b.size : VkDeviceSize(range_bytes);
        return info;
    }
};

VulkanBackend::VulkanBackend() : impl_(new Impl()) {}

VulkanBackend::~VulkanBackend()
{
    Shutdown();
    delete impl_;
}

bool VulkanBackend::IsInitialized() const { return impl_->initialized; }
const BackendInfo& VulkanBackend::Info() const { return impl_->info; }
const std::string& VulkanBackend::LastError() const { return impl_->last_error; }

bool VulkanBackend::Initialize()
{
    return Initialize(-1);
}

std::vector<std::string> VulkanBackend::EnumerateDevices()
{
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "BurstMerge";
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    VkInstance inst;
    if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) return {};

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(inst, &count, nullptr);
    std::vector<VkPhysicalDevice> gpus(count);
    if (count) vkEnumeratePhysicalDevices(inst, &count, gpus.data());

    std::vector<std::string> names;
    for (auto ph : gpus)
    {
        VkPhysicalDeviceProperties prop;
        vkGetPhysicalDeviceProperties(ph, &prop);
        uint32_t qfc = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(ph, &qfc, nullptr);
        std::vector<VkQueueFamilyProperties> qfp(qfc);
        vkGetPhysicalDeviceQueueFamilyProperties(ph, &qfc, qfp.data());
        bool has_compute = false;
        for (uint32_t i = 0; i < qfc; ++i)
            if (qfp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { has_compute = true; break; }
        if (has_compute)
            names.push_back(prop.deviceName);
    }
    vkDestroyInstance(inst, nullptr);
    return names;
}

bool VulkanBackend::Initialize(int device_index)
{
    Impl& d = *impl_;
    if (d.initialized) return true;

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "BurstMerge";
    app.applicationVersion = 1;
    app.pEngineName = "BurstMerge";
    app.engineVersion = 1;
    app.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
#ifdef NDEBUG
    ici.enabledLayerCount = 0;
#else
    const char* dbg = "VK_LAYER_KHRONOS_validation";
    uint32_t lc = 0;
    vkEnumerateInstanceLayerProperties(&lc, nullptr);
    std::vector<VkLayerProperties> layers(lc);
    if (lc) vkEnumerateInstanceLayerProperties(&lc, layers.data());
    bool have_dbg = false;
    for (auto& lp : layers) if (std::strcmp(lp.layerName, dbg) == 0) have_dbg = true;
    if (have_dbg)
    {
        ici.enabledLayerCount = 1;
        ici.ppEnabledLayerNames = &dbg;
    }
#endif

    VkResult r = vkCreateInstance(&ici, nullptr, &d.instance);
    if (r != VK_SUCCESS)
    {
        impl_->last_error = "vkCreateInstance failed";
        return false;
    }

    uint32_t gpu_count = 0;
    vkEnumeratePhysicalDevices(d.instance, &gpu_count, nullptr);
    if (gpu_count == 0) { impl_->last_error = "No Vulkan GPU"; return false; }
    std::vector<VkPhysicalDevice> gpus(gpu_count);
    vkEnumeratePhysicalDevices(d.instance, &gpu_count, gpus.data());

    // Helper: find the best compute queue family index on a physical device.
    auto find_compute_queue = [](VkPhysicalDevice phd, int& qidx) {
        uint32_t qfc = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(phd, &qfc, nullptr);
        std::vector<VkQueueFamilyProperties> qfp(qfc);
        vkGetPhysicalDeviceQueueFamilyProperties(phd, &qfc, qfp.data());
        qidx = -1;
        for (uint32_t i = 0; i < qfc; ++i)
            if (qfp[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
            {
                qidx = int(i);
                if (!(qfp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) break;
            }
    };

    if (device_index >= 0 && device_index < int(gpu_count))
    {
        // User-specified device.
        int qidx = -1;
        find_compute_queue(gpus[device_index], qidx);
        if (qidx < 0) { impl_->last_error = "Selected GPU has no compute queue"; return false; }
        VkPhysicalDeviceProperties prop;
        vkGetPhysicalDeviceProperties(gpus[device_index], &prop);
        d.physical = gpus[device_index];
        d.queue_family = uint32_t(qidx);
        d.info.device_name = prop.deviceName;
        d.info.api_version = prop.apiVersion;
    }
    else
    {
        // Auto-select best device (discrete GPU preferred).
        int best_score = -1;
        for (auto ph : gpus)
        {
            VkPhysicalDeviceProperties prop;
            vkGetPhysicalDeviceProperties(ph, &prop);
            int qidx = -1;
            find_compute_queue(ph, qidx);
            if (qidx < 0) continue;
            int score = 0;
            if (prop.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score += 100;
            else if (prop.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score += 10;
            score += int(prop.limits.maxComputeWorkGroupInvocations);
            if (score > best_score)
            {
                best_score = score;
                d.physical = ph;
                d.queue_family = uint32_t(qidx);
                d.info.device_name = prop.deviceName;
                d.info.api_version = prop.apiVersion;
            }
        }
    }
    if (d.physical == VK_NULL_HANDLE) { impl_->last_error = "No compute-capable GPU"; return false; }
    vkGetPhysicalDeviceMemoryProperties(d.physical, &d.mem_props);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi{};
    qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = d.queue_family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    VkPhysicalDeviceFeatures pdf{};
    vkGetPhysicalDeviceFeatures(d.physical, &pdf);
    // All shaders use single-precision float only. shaderFloat64 is intentionally
    // NOT enabled — no shader declares or uses 'double' types.
    VkPhysicalDeviceFeatures want{};
    want.shaderInt64 = pdf.shaderInt64;
    VkDeviceCreateInfo di{};
    di.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.pEnabledFeatures = &want;
    di.enabledExtensionCount = 0;
    VkResult dr = vkCreateDevice(d.physical, &di, nullptr, &d.device);
    if (dr != VK_SUCCESS) { impl_->last_error = "vkCreateDevice failed"; return false; }
    vkGetDeviceQueue(d.device, d.queue_family, 0, &d.queue);

    VkCommandPoolCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = d.queue_family;
    if (vkCreateCommandPool(d.device, &cpi, nullptr, &d.cmd_pool) != VK_SUCCESS)
    { impl_->last_error = "vkCreateCommandPool failed"; return false; }

    // descriptor set layout: bindings 0..7 storage buffer, 8 uniform buffer
    std::vector<VkDescriptorSetLayoutBinding> lbs;
    for (uint32_t i = 0; i < kMaxStorageBindings; ++i)
    {
        VkDescriptorSetLayoutBinding b{};
        b.binding = i;
        b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        lbs.push_back(b);
    }
    {
        VkDescriptorSetLayoutBinding b{};
        b.binding = kUboBinding;
        b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        lbs.push_back(b);
    }
    VkDescriptorSetLayoutCreateInfo sli{};
    sli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    sli.bindingCount = uint32_t(lbs.size());
    sli.pBindings = lbs.data();
    if (vkCreateDescriptorSetLayout(d.device, &sli, nullptr, &d.set_layout) != VK_SUCCESS)
    { impl_->last_error = "vkCreateDescriptorSetLayout failed"; return false; }

    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.offset = 0;
    pcr.size = kPushConstantBytes;
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &d.set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(d.device, &pli, nullptr, &d.pipeline_layout) != VK_SUCCESS)
    { impl_->last_error = "vkCreatePipelineLayout failed"; return false; }

    VkDescriptorPoolSize pool_sizes[2];
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_sizes[0].descriptorCount = kMaxFrameDispatches * kMaxStorageBindings;
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    pool_sizes[1].descriptorCount = kMaxFrameDispatches;
    VkDescriptorPoolCreateInfo dpi{};
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpi.maxSets = kMaxFrameDispatches;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes = pool_sizes;
    if (vkCreateDescriptorPool(d.device, &dpi, nullptr, &d.desc_pool) != VK_SUCCESS)
    { impl_->last_error = "vkCreateDescriptorPool failed"; return false; }

    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = d.cmd_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(d.device, &ai, &d.frame_cb) != VK_SUCCESS)
    { impl_->last_error = "vkAllocateCommandBuffers failed"; return false; }

    d.info.valid = true;
    d.initialized = true;
    std::fprintf(stderr, "[Vulkan] Initialized on '%s' (queue family %u)\n",
                 d.info.device_name.c_str(), d.queue_family);
    return true;
}

void VulkanBackend::Shutdown()
{
    Impl& d = *impl_;
    if (!d.initialized) return;
    if (d.recording) FlushFrame();
    vkQueueWaitIdle(d.queue);

    for (auto name : d.created_pipelines)
        if (d.pipelines[name]) vkDestroyPipeline(d.device, d.pipelines[name], nullptr);
    d.pipelines.clear();
    d.created_pipelines.clear();

    for (auto& kv : d.buffers)
    {
        std::fprintf(stderr, "[VRAM]   leak: handle=%llu size=%.1fMB floats=%u host=%d\n",
            static_cast<unsigned long long>(kv.first),
            static_cast<double>(kv.second.size)/(1024.0*1024.0),
            kv.second.float_count, kv.second.host_visible ? 1 : 0);
        if (kv.second.mapped) vkUnmapMemory(d.device, kv.second.mem);
        if (kv.second.buf) vkDestroyBuffer(d.device, kv.second.buf, nullptr);
        if (kv.second.mem) vkFreeMemory(d.device, kv.second.mem, nullptr);
    }
    d.buffers.clear();

    if (d.frame_cb) vkFreeCommandBuffers(d.device, d.cmd_pool, 1, &d.frame_cb);
    if (d.desc_pool) vkDestroyDescriptorPool(d.device, d.desc_pool, nullptr);
    if (d.pipeline_layout) vkDestroyPipelineLayout(d.device, d.pipeline_layout, nullptr);
    if (d.set_layout) vkDestroyDescriptorSetLayout(d.device, d.set_layout, nullptr);
    if (d.cmd_pool) vkDestroyCommandPool(d.device, d.cmd_pool, nullptr);
    if (d.device) vkDestroyDevice(d.device, nullptr);
    if (d.instance) vkDestroyInstance(d.instance, nullptr);
    std::fprintf(stderr, "[VRAM] peak: %.1f MB (%llu buffers), leaked: %.1f MB (%llu buffers)\n",
        static_cast<double>(d.vram_peak) / (1024.0*1024.0),
        static_cast<unsigned long long>(d.vram_peak_count),
        static_cast<double>(d.vram_live) / (1024.0*1024.0),
        static_cast<unsigned long long>(d.vram_live_count));
    d.initialized = false;
}

static VulkanBackend::Impl::Buffer alloc_buffer(VulkanBackend::Impl& d,
                                                 VkDeviceSize size,
                                                 VkBufferUsageFlags usage,
                                                 VkMemoryPropertyFlags props,
                                                 bool host_visible,
                                                 bool ubo)
{
    VulkanBackend::Impl::Buffer b;
    b.size = size;
    b.host_visible = host_visible;
    b.ubo = ubo;
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(d.device, &bi, nullptr, &b.buf) != VK_SUCCESS) return b;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(d.device, b.buf, &req);
    uint32_t type = 0;
    if (!FindMemoryType(d.mem_props, req.memoryTypeBits, props, type)) return b;
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (vkAllocateMemory(d.device, &mai, nullptr, &b.mem) != VK_SUCCESS) return b;
    vkBindBufferMemory(d.device, b.buf, b.mem, 0);
    if (host_visible)
    {
        if (vkMapMemory(d.device, b.mem, 0, req.size, 0, &b.mapped) == VK_SUCCESS)
        {
            if (props & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) { /* coherent */ }
        }
    }
    return b;
}

uint64_t VulkanBackend::CreateBuffer(uint32_t float_count)
{
    Impl& d = *impl_;
    VkDeviceSize sz = VkDeviceSize(float_count) * 4;
    Impl::Buffer b = alloc_buffer(d, sz,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false, false);
    b.float_count = float_count;
    uint64_t h = d.next_handle++;
    d.buffers[h] = b;
    d.vram_live += b.size; d.vram_live_count += 1;
    if (d.vram_live > d.vram_peak) { d.vram_peak = d.vram_live; d.vram_peak_count = d.vram_live_count; }
    return h;
}

uint64_t VulkanBackend::CreateHostBuffer(uint32_t float_count)
{
    Impl& d = *impl_;
    VkDeviceSize sz = VkDeviceSize(float_count) * 4;
    Impl::Buffer b = alloc_buffer(d, sz,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        true, false);
    b.float_count = float_count;
    uint64_t h = d.next_handle++;
    d.buffers[h] = b;
    d.vram_live += b.size; d.vram_live_count += 1;
    if (d.vram_live > d.vram_peak) { d.vram_peak = d.vram_live; d.vram_peak_count = d.vram_live_count; }
    return h;
}

uint64_t VulkanBackend::CreateBufferFromFloats(const float* data, uint32_t float_count)
{
    Impl& d = *impl_;
    uint64_t h = CreateBuffer(float_count);
    Impl::Buffer& b = d.buffers[h];
    if (!data || float_count == 0) return h;
    // staging
    VkDeviceSize sz = b.size;
    Impl::Buffer staging = alloc_buffer(d, sz,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        true, false);
    std::memcpy(staging.mapped, data, size_t(sz));
    VkCommandBuffer cb = d.one_shot_begin();
    VkBufferCopy reg{}; reg.size = sz;
    vkCmdCopyBuffer(cb, staging.buf, b.buf, 1, &reg);
    d.one_shot_end_wait(cb);
    if (staging.mapped) vkUnmapMemory(d.device, staging.mem);
    vkDestroyBuffer(d.device, staging.buf, nullptr);
    vkFreeMemory(d.device, staging.mem, nullptr);
    return h;
}

uint64_t VulkanBackend::CreateUbo(const void* data, uint32_t bytes)
{
    Impl& d = *impl_;
    VkDeviceSize sz = VkDeviceSize(bytes);
    Impl::Buffer b = alloc_buffer(d, sz,
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        true, true);
    if (data && b.mapped) std::memcpy(b.mapped, data, bytes);
    b.float_count = 0;
    uint64_t h = d.next_handle++;
    d.buffers[h] = b;
    d.vram_live += b.size; d.vram_live_count += 1;
    if (d.vram_live > d.vram_peak) { d.vram_peak = d.vram_live; d.vram_peak_count = d.vram_live_count; }
    return h;
}

void VulkanBackend::UpdateUbo(uint64_t handle, const void* data, uint32_t bytes)
{
    Impl& d = *impl_;
    auto it = d.buffers.find(handle);
    if (it == d.buffers.end()) return;
    if (it->second.mapped) std::memcpy(it->second.mapped, data, bytes);
}

void VulkanBackend::UploadFloats(uint64_t handle, const float* data, uint32_t float_count)
{
    Impl& d = *impl_;
    auto it = d.buffers.find(handle);
    if (it == d.buffers.end()) return;
    Impl::Buffer& b = it->second;
    VkDeviceSize sz = VkDeviceSize(float_count) * 4;
    Impl::Buffer staging = alloc_buffer(d, sz,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        true, false);
    std::memcpy(staging.mapped, data, size_t(sz));
    VkCommandBuffer cb = d.one_shot_begin();
    VkBufferCopy reg{}; reg.size = sz;
    vkCmdCopyBuffer(cb, staging.buf, b.buf, 1, &reg);
    d.one_shot_end_wait(cb);
    if (staging.mapped) vkUnmapMemory(d.device, staging.mem);
    vkDestroyBuffer(d.device, staging.buf, nullptr);
    vkFreeMemory(d.device, staging.mem, nullptr);
}

void VulkanBackend::DownloadFloats(uint64_t handle, float* out, uint32_t float_count)
{
    Impl& d = *impl_;
    auto it = d.buffers.find(handle);
    if (it == d.buffers.end()) return;
    Impl::Buffer& b = it->second;
    if (b.host_visible && b.mapped)
    {
        std::memcpy(out, b.mapped, size_t(float_count) * 4);
        return;
    }
    VkDeviceSize sz = VkDeviceSize(float_count) * 4;

    if (d.download_staging.capacity < sz)
    {
        // On NVIDIA GPUs (tested RTX 3080, driver 560+), there are multiple
        // HOST_VISIBLE memory types. The DEVICE_LOCAL|HOST_VISIBLE type (PCIe
        // BAR) is uncached — CPU reads top out at ~100 MB/s. The HOST_CACHED
        // type (system RAM) gives ~15 GB/s. We MUST prefer HOST_CACHED to
        // avoid a 50x slowdown on the final merged-plane readback (measured:
        // 586ms -> 3ms for 56MB). If no HOST_CACHED type exists (some AMD/
        // Intel IGP setups), fallback accepts any HOST_VISIBLE type.
        if (d.download_staging.mapped) vkUnmapMemory(d.device, d.download_staging.mem);
        if (d.download_staging.buf) vkDestroyBuffer(d.device, d.download_staging.buf, nullptr);
        if (d.download_staging.mem) vkFreeMemory(d.device, d.download_staging.mem, nullptr);
        d.download_staging = {};
        VkDeviceSize cap = sz;
        if (cap < 1024 * 1024) cap = 1024 * 1024;
        d.download_staging.capacity = cap;
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = cap;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCreateBuffer(d.device, &bci, nullptr, &d.download_staging.buf);
        VkMemoryRequirements mrq;
        vkGetBufferMemoryRequirements(d.device, d.download_staging.buf, &mrq);
        uint32_t memType = UINT32_MAX;
        for (uint32_t pass = 0; pass < 2; ++pass)
        {
            for (uint32_t i = 0; i < d.mem_props.memoryTypeCount; ++i)
            {
                if (!(mrq.memoryTypeBits & (1u << i))) continue;
                auto fl = d.mem_props.memoryTypes[i].propertyFlags;
                if (!(fl & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) continue;
                if (pass == 0 && !(fl & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) continue;
                memType = i;
                break;
            }
            if (memType != UINT32_MAX) break;
        }
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = mrq.size;
        mai.memoryTypeIndex = memType;
        vkAllocateMemory(d.device, &mai, nullptr, &d.download_staging.mem);
        vkBindBufferMemory(d.device, d.download_staging.buf, d.download_staging.mem, 0);
        vkMapMemory(d.device, d.download_staging.mem, 0, cap, 0, &d.download_staging.mapped);
    }

    VkCommandBuffer cb = d.one_shot_begin();
    VkBufferCopy reg{}; reg.size = sz;
    vkCmdCopyBuffer(cb, b.buf, d.download_staging.buf, 1, &reg);
    d.one_shot_end_wait(cb);
    std::memcpy(out, d.download_staging.mapped, size_t(sz));
}

void VulkanBackend::DownloadUints(uint64_t handle, uint32_t* out, uint32_t uint_count)
{
    DownloadFloats(handle, reinterpret_cast<float*>(out), uint_count);
}

void VulkanBackend::FillFloat(uint64_t handle, float value)
{
    Impl& d = *impl_;
    auto it = d.buffers.find(handle);
    if (it == d.buffers.end()) return;
    Impl::Buffer& b = it->second;
    ShaderPC pc{};
    pc.w = int32_t(b.float_count);
    pc.h = 1;
    pc.channels = 1;
    pc.f0 = value;
    Binding bind{0, handle, 0};
    if (d.recording) FlushFrame();
    BeginFrame();
    Dispatch("fill", pc, (uint32_t(b.float_count) + 255u) / 256u, 1, 1, &bind, 1);
    FlushFrame();
}

void VulkanBackend::CopyBufferRegion(uint64_t dst, uint32_t dst_offset_floats,
                                     uint64_t src, uint32_t float_count)
{
    Impl& d = *impl_;
    auto itd = d.buffers.find(dst);
    auto its = d.buffers.find(src);
    if (itd == d.buffers.end() || its == d.buffers.end()) return;
    if (!d.recording) BeginFrame();
    VkBufferCopy reg{};
    reg.srcOffset = 0;
    reg.dstOffset = VkDeviceSize(dst_offset_floats) * 4;
    reg.size = VkDeviceSize(float_count) * 4;
    vkCmdCopyBuffer(d.frame_cb, its->second.buf, itd->second.buf, 1, &reg);
    VkMemoryBarrier mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(d.frame_cb,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &mb, 0, nullptr, 0, nullptr);
}

uint32_t VulkanBackend::BufferFloatCount(uint64_t handle) const
{
    auto it = impl_->buffers.find(handle);
    return it == impl_->buffers.end() ? 0 : it->second.float_count;
}

void VulkanBackend::DestroyBuffer(uint64_t handle)
{
    Impl& d = *impl_;
    auto it = d.buffers.find(handle);
    if (it == d.buffers.end()) return;
    if (d.recording) FlushFrame();
    d.vram_live -= it->second.size;
    d.vram_live_count -= 1;
    if (it->second.mapped) vkUnmapMemory(d.device, it->second.mem);
    if (it->second.buf) vkDestroyBuffer(d.device, it->second.buf, nullptr);
    if (it->second.mem) vkFreeMemory(d.device, it->second.mem, nullptr);
    d.buffers.erase(it);
}

void VulkanBackend::DeferredDestroy(uint64_t handle)
{
    impl_->pending_deferred.push_back(handle);
}

void VulkanBackend::BeginFrame()
{
    Impl& d = *impl_;
    if (d.recording) return;
    vkResetDescriptorPool(d.device, d.desc_pool, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(d.frame_cb, &bi);
    d.recording = true;
    d.frame_dispatches = 0;
}

void VulkanBackend::FlushFrame()
{
    Impl& d = *impl_;
    if (!d.recording) return;
    vkEndCommandBuffer(d.frame_cb);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &d.frame_cb;
    vkQueueSubmit(d.queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(d.queue);
    vkResetCommandBuffer(d.frame_cb, 0);
    d.recording = false;
    d.frame_dispatches = 0;

    // GPU is now idle — safe to process deferred destructions.
    // This is the only place pending_deferred is drained, so callers
    // can queue frees at any time without worrying about sync.
    if (!d.pending_deferred.empty())
    {
        for (uint64_t h : d.pending_deferred)
        {
            auto it = d.buffers.find(h);
            if (it == d.buffers.end()) continue;
            d.vram_live -= it->second.size;
            d.vram_live_count -= 1;
            if (it->second.mapped) vkUnmapMemory(d.device, it->second.mem);
            if (it->second.buf) vkDestroyBuffer(d.device, it->second.buf, nullptr);
            if (it->second.mem) vkFreeMemory(d.device, it->second.mem, nullptr);
            d.buffers.erase(it);
        }
        d.pending_deferred.clear();
    }
}

void VulkanBackend::Synchronize()
{
    if (impl_->recording) FlushFrame();
    else vkQueueWaitIdle(impl_->queue);
}

void VulkanBackend::Dispatch(const char* shader, const ShaderPC& pc,
                             uint32_t gx, uint32_t gy, uint32_t gz,
                             const Binding* bindings, uint32_t num_bindings)
{
    Impl& d = *impl_;
    if (!d.recording) BeginFrame();
    if (d.frame_dispatches >= kMaxFrameDispatches) { FlushFrame(); BeginFrame(); }

    VkPipeline pipe = d.get_pipeline(shader);
    if (pipe == VK_NULL_HANDLE) return;

    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = d.desc_pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &d.set_layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(d.device, &dsai, &set) != VK_SUCCESS)
    {
        // pool exhausted mid-frame: flush, reset, retry
        FlushFrame();
        BeginFrame();
        vkResetDescriptorPool(d.device, d.desc_pool, 0);
        if (vkAllocateDescriptorSets(d.device, &dsai, &set) != VK_SUCCESS) return;
    }

    VkDescriptorBufferInfo infos[9];
    VkWriteDescriptorSet writes[9];
    uint32_t nw = 0;
    for (uint32_t i = 0; i < num_bindings && i < 9; ++i)
    {
        auto it = d.buffers.find(bindings[i].handle);
        if (it == d.buffers.end()) continue;
        infos[nw] = d.make_info(it->second, bindings[i].range);
        writes[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[nw].pNext = nullptr;
        writes[nw].dstSet = set;
        writes[nw].dstBinding = bindings[i].binding;
        writes[nw].dstArrayElement = 0;
        writes[nw].descriptorCount = 1;
        writes[nw].descriptorType = (bindings[i].binding == kUboBinding)
            ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
            : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[nw].pBufferInfo = &infos[nw];
        ++nw;
    }
    if (nw) vkUpdateDescriptorSets(d.device, nw, writes, 0, nullptr);

    vkCmdBindPipeline(d.frame_cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(d.frame_cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                            d.pipeline_layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(d.frame_cb, d.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, kPushConstantBytes, &pc);
    vkCmdDispatch(d.frame_cb, gx ? gx : 1, gy ? gy : 1, gz ? gz : 1);

    VkMemoryBarrier mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(d.frame_cb,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &mb, 0, nullptr, 0, nullptr);
    ++d.frame_dispatches;
}

} // namespace vulkan
} // namespace burstmerge
