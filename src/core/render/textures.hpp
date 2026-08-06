#pragma once

#include "common/singleton.hpp"
#include "core/all_extern.hpp"
#include "core/vulkan/all_core_vulkan.hpp"

#include <functional>
#include <map>
#include <mutex>
#include <vector>

class Framework;
class Emission;

class ImageBufferCache;

class Textures : public SharedObject<Textures> {
    friend class Emission;

  public:
    struct SubmittedUploadBatch {
        std::shared_ptr<vk::Fence> fence;
        std::shared_ptr<vk::CommandBuffer> commandBuffer;
        std::vector<std::shared_ptr<vk::HostVisibleBuffer>> stagingBuffers;
    };

    Textures(std::shared_ptr<Framework> framework);

    void reset();
    void resetFrame();
    uint32_t allocateTexture();
    void initializeTexture(uint32_t id, uint32_t maxLevel, uint32_t width, uint32_t height, VkFormat format);
    // Off-screen render-target COLOR image (e.g. GuiItemAtlas): registered + bound into the bindless
    // array under its GL id like a normal 2D texture, but created with COLOR_ATTACHMENT usage so the
    // draw-replay path can render INTO it (see UIModuleContext::beginTargetDraw). Single mip.
    void initializeRenderTarget(uint32_t id, uint32_t width, uint32_t height, VkFormat format);
    // Cube textures (the panorama) live in a separate registry from the 2D textures_ because they are
    // sampled as samplerCube through the overlay cube bindless binding, not sampler2D. prepareCubeImage
    // creates a 6-layer cube-compatible image + sampler and binds it into the overlay cube array at its
    // GL id; uploadCube copies all six stacked faces from the source NativeImage in one shot.
    void prepareCubeImage(uint32_t id, uint32_t maxLevel, uint32_t faceWidth, uint32_t faceHeight,
                          VkFormat format);
    void uploadCube(uint32_t id, uint8_t *src);
    void setSamplingMode(uint32_t id, VkFilter samplingMode, VkSamplerMipmapMode mipmapMode);
    void setAddressMode(uint32_t id, VkSamplerAddressMode addressMode);
    void queueUpload(uint8_t *srcPointer,
                     uint32_t srcSizeInBytes,
                     uint32_t srcRowPixels,
                     uint32_t dstId,
                     int srcOffsetX,
                     int srcOffsetY,
                     int dstOffsetX,
                     int dstOffsetY,
                     uint32_t width,
                     uint32_t height,
                     uint32_t level);
    void performQueuedUpload();
    void bindAllTextures();
    std::shared_ptr<vk::DeviceLocalImage> texture(uint32_t id);
    std::shared_ptr<vk::Sampler> sampler(uint32_t id);
    std::shared_ptr<Emission> emission();
    void releaseEmission();

    std::map<uint32_t, std::shared_ptr<vk::DeviceLocalImage>> textures_;
    std::map<uint32_t, std::shared_ptr<vk::Sampler>> samplers;
    // Cube-texture registry, keyed by GL id like textures_/samplers (see prepareCubeImage).
    std::map<uint32_t, std::shared_ptr<vk::DeviceLocalImage>> cubeTextures_;
    std::map<uint32_t, std::shared_ptr<vk::Sampler>> cubeSamplers_;
    std::shared_ptr<Emission> emission_;
    // Mod-internal texture ids allocated top-down from the 4096-entry descriptor array (see reset()).
    uint32_t nextID = 4095;
    // Recycled-GL-id re-inits since the last retainer drain (bounds reload-time pile-up).
    uint32_t reinitSinceDrain_ = 0;
    std::recursive_mutex mtx_;

    std::map<uint32_t, std::shared_ptr<ImageBufferCache>> caches_;
    std::shared_ptr<std::map<uint32_t, std::vector<VkBufferImageCopy>>> uploadQueue_;
    std::vector<SubmittedUploadBatch> submittedUploadBatches_;
    std::vector<std::shared_ptr<vk::CommandBuffer>> freeUploadCommandBuffers_;
    std::vector<std::shared_ptr<vk::HostVisibleBuffer>> freeUploadStagingBuffers_;
    std::vector<std::shared_ptr<vk::Fence>> freeUploadFences_;
    size_t queuedUploadBytes_ = 0;

  private:
    static constexpr size_t UPLOAD_FLUSH_THRESHOLD = 64 * 1024 * 1024;
    // Flush after this many distinct queued textures even below the byte threshold (bounds the
    // first-frame flush after a resource reload -- see queueUpload).
    static constexpr size_t UPLOAD_FLUSH_TEXTURE_COUNT = 128;

    std::shared_ptr<vk::HostVisibleBuffer> acquireUploadStagingBuffer(size_t minSize);
    std::shared_ptr<vk::Fence> acquireUploadFence();
    void collectCompletedUploadsImpl();
    void flushQueuedUploadImpl();
};

class ImageBufferCache : public SharedObject<ImageBufferCache> {
  public:
    constexpr static size_t BASE_SIZE = 16 * 1024; // 1KB
    constexpr static size_t ALIGNMENT = 4;

    ImageBufferCache(std::shared_ptr<vk::VMA> vma, std::shared_ptr<vk::Device> device, uint32_t frameNum);
    ~ImageBufferCache();

    size_t append(void *src, size_t size);
    void flush();
    void reset();
    std::shared_ptr<vk::HostVisibleBuffer> detachCurrentBuffer();
    void replaceCurrentBuffer(std::shared_ptr<vk::HostVisibleBuffer> buffer);

    VkBuffer &vkBuffer();

  private:
    std::shared_ptr<vk::VMA> vma_;
    std::shared_ptr<vk::Device> device_;

    uint32_t current_ = 0;
    std::vector<size_t> capacities_;
    std::vector<size_t> bases_;
    std::vector<std::shared_ptr<vk::HostVisibleBuffer>> caches_;
};
