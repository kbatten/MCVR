#include "core/render/textures.hpp"

#include "core/render/emission.hpp"
#include "core/render/modules/ui_module.hpp"
#include "core/render/pipeline.hpp"
#include "core/render/render_framework.hpp"
#include "core/render/renderer.hpp"

std::ostream &texturesCout() {
    return std::cout << "[Textures] ";
}

std::ostream &texturesCerr() {
    return std::cerr << "[Textures] ";
}

Textures::Textures(std::shared_ptr<Framework> framework) {}

void Textures::reset() {
    textures_.clear();
    if (emission_ != nullptr) { emission_->reset(); }
    // Mod-internal texture ids are allocated from the top of the 4096-entry descriptor array
    // downward so they never collide with MC-owned GL ids (assigned from the bottom up).
    nextID = 4095;
}

void Textures::resetFrame() {
    auto framework = Renderer::instance().framework();

    collectCompletedUploadsImpl();

    framework->frameResourceRetainer().retain(uploadQueue_);
    uploadQueue_ = std::make_shared<std::map<uint32_t, std::vector<VkBufferImageCopy>>>();
    queuedUploadBytes_ = 0;

    for (auto &entry : caches_) {
        auto &cache = entry.second;
        cache->reset();
    }
}

uint32_t Textures::allocateTexture() {
    std::scoped_lock lck(mtx_, Renderer::instance().framework()->recreateMtx());

    textures_.emplace(std::make_pair(nextID, nullptr));
    samplers.emplace(std::make_pair(nextID, nullptr));
    return nextID--;  // allocate downward from the top of the descriptor array (see reset())
}

void Textures::initializeTexture(uint32_t id, uint32_t maxLevel, uint32_t width, uint32_t height, VkFormat format) {
    auto framework = Renderer::instance().framework();
    auto device = framework->device();
    auto vma = framework->vma();

    std::scoped_lock lck(mtx_, Renderer::instance().framework()->recreateMtx());

    texturesCerr() << "initTex id=" << id << " " << width << "x" << height << " maxLevel=" << maxLevel
                   << " format=" << format << std::endl;

    auto textureIter = textures_.find(id);
    if (textureIter == textures_.end()) {
        // 26.2: texture ids are MC-owned GL ids (GlTexture.glId()) that were never handed out by
        // allocateTexture(), so allocate the slot on demand instead of failing. allocateTexture()
        // hands out mod-internal ids from the top of the 4096-entry descriptor array downward, so
        // GL ids (assigned from the bottom up by the driver) do not collide with them.
        textures_.emplace(id, nullptr);
        samplers.emplace(id, nullptr);
    } else if (textures_[id] != nullptr) {
        // Re-init of a recycled GL texture id (MC deletes a texture and glGenTextures hands the same
        // id back for a new one). The old texture+sampler are retained below for GPU safety, but
        // during resource reload no frames are presented, so the retainer is never drained and they
        // pile up until memory / maxMemoryAllocationCount is exhausted. Bound the pile-up.
        if (++reinitSinceDrain_ >= 256) {
            reinitSinceDrain_ = 0;
            texturesCerr() << "[Drain] releaseAll (256 re-inits)" << std::endl;
            framework->waitDeviceIdle();
            framework->frameResourceRetainer().releaseAll();
        }
    }

    // Uploads queued for this id were sized against the image we are about to replace, but
    // flushQueuedUploadImpl() resolves textures_[id] at *flush* time. Leaving them queued replays
    // the old texture's extent into the new (often smaller) image -- an out-of-bounds GPU write that
    // faults the device. MC re-uploads the new texture's contents itself, so dropping them is safe.
    if (uploadQueue_ != nullptr) {
        auto queuedIter = uploadQueue_->find(id);
        if (queuedIter != uploadQueue_->end()) {
            texturesCerr() << "dropping " << queuedIter->second.size() << " stale queued upload(s) for re-initialized id="
                           << id << std::endl;
            uploadQueue_->erase(queuedIter);
        }
    }

    framework->frameResourceRetainer().retain(textures_[id]);
#ifdef DEBUG
    if (textures_[id] != nullptr) { std::cout << "Textrue reinitialized: " << id << std::endl; }
#endif
    textures_[id] = vk::DeviceLocalImage::create(device, vma, false, maxLevel, width, height, 1, format,
                                                 VK_IMAGE_USAGE_SAMPLED_BIT, 0, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, 0
#ifdef DEBUG
                                                 ,
                                                 "Texture " + std::to_string(id)
#endif
    );

    auto samplerIter = samplers.find(id);
    if (samplerIter == samplers.end()) {
        texturesCerr() << "The given texture id: " << id << " is not allocated for sampler" << std::endl;
        exit(EXIT_FAILURE);
    }
    framework->frameResourceRetainer().retain(samplers[id]);
    samplers[id] =
        vk::Sampler::create(device, VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_REPEAT);

    Renderer::instance().framework()->pipeline()->bindTexture(samplers[id], textures_[id], id);
}

void Textures::prepareCubeImage(uint32_t id, uint32_t maxLevel, uint32_t faceWidth, uint32_t faceHeight,
                                VkFormat format) {
    auto framework = Renderer::instance().framework();
    auto device = framework->device();
    auto vma = framework->vma();

    std::scoped_lock lck(mtx_, framework->recreateMtx());

    texturesCerr() << "prepareCubeImage id=" << id << " face=" << faceWidth << "x" << faceHeight
                   << " maxLevel=" << maxLevel << " format=" << format << std::endl;

    // Retain any previous cube (recycled GL id) so an in-flight frame still sampling it stays valid.
    auto existing = cubeTextures_.find(id);
    if (existing != cubeTextures_.end()) { framework->frameResourceRetainer().retain(existing->second); }
    auto existingSampler = cubeSamplers_.find(id);
    if (existingSampler != cubeSamplers_.end()) {
        framework->frameResourceRetainer().retain(existingSampler->second);
    }

    // persistStaging so uploadCube can memcpy the six faces straight into the image's own staging
    // buffer; depth 1, layer 6, cube-compatible so image.cpp gives it a VK_IMAGE_VIEW_TYPE_CUBE view.
    cubeTextures_[id] = vk::DeviceLocalImage::create(
        device, vma, true, maxLevel, faceWidth, faceHeight, 1, 6, format, VK_IMAGE_USAGE_SAMPLED_BIT, 0,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT
#ifdef DEBUG
        ,
        "Cube texture " + std::to_string(id)
#endif
    );
    // Cubemaps sample linearly with clamp-to-edge so the six faces meet without seams.
    cubeSamplers_[id] = vk::Sampler::create(device, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);

    framework->pipeline()->bindCubeTexture(cubeSamplers_[id], cubeTextures_[id], id);
}

void Textures::uploadCube(uint32_t id, uint8_t *src) {
    auto framework = Renderer::instance().framework();
    auto device = framework->device();

    std::scoped_lock lck(mtx_, framework->recreateMtx());

    auto it = cubeTextures_.find(id);
    if (it == cubeTextures_.end() || it->second == nullptr) {
        texturesCerr() << "uploadCube: cube id " << id << " is not prepared" << std::endl;
        return;
    }
    auto image = it->second;

    // Copy all six stacked faces into the image's staging buffer (uploadToStagingBuffer sizes itself
    // for the 6-layer image), then transition, copy every layer in one region (uploadToImage sets
    // layerCount = 6), and transition to shader-read. One-time load at resource reload, so a
    // device-idle wait is cheaper than threading a fence through -- same pattern as the fallbacks.
    image->uploadToStagingBuffer(src);

    auto mainQueueIndex = framework->physicalDevice()->mainQueueIndex();
    auto cmdBuffer = vk::CommandBuffer::create(device, framework->mainCommandPool());
    cmdBuffer->begin();

    cmdBuffer->barriersBufferImage({}, {{
                                           .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                                           .srcAccessMask = VK_ACCESS_2_NONE,
                                           .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                           .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                           .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                           .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                           .srcQueueFamilyIndex = mainQueueIndex,
                                           .dstQueueFamilyIndex = mainQueueIndex,
                                           .image = image,
                                           .subresourceRange = vk::wholeColorSubresourceRange,
                                       }});

    image->uploadToImage(cmdBuffer);

    cmdBuffer->barriersBufferImage({}, {{
                                           .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                           .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                                           VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                                           .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                           .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                           .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                           .srcQueueFamilyIndex = mainQueueIndex,
                                           .dstQueueFamilyIndex = mainQueueIndex,
                                           .image = image,
                                           .subresourceRange = vk::wholeColorSubresourceRange,
                                       }});
    image->imageLayout() = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    cmdBuffer->end();
    cmdBuffer->submitMainQueueIndividual(device);
    framework->waitDeviceIdle();
}

void Textures::setSamplingMode(uint32_t id, VkFilter samplingMode, VkSamplerMipmapMode mipmapMode) {
    auto device = Renderer::instance().framework()->device();

    std::scoped_lock lck(mtx_, Renderer::instance().framework()->recreateMtx());

    auto samplerIter = samplers.find(id);
    if (samplerIter == samplers.end()) {
        texturesCerr() << "The given texture id: " << id << " is not allocated for sampler" << std::endl;
        exit(EXIT_FAILURE);
    }
    if (samplers[id]->vkSamplingMode() != samplingMode) {
        VkSamplerAddressMode addressMode = samplers[id]->vkAddressMode();

        auto framework = Renderer::instance().framework();
        framework->frameResourceRetainer().retain(samplers[id]);
        samplers[id] = vk::Sampler::create(device, samplingMode, mipmapMode, addressMode);
    }

    Renderer::instance().framework()->pipeline()->bindTexture(samplers[id], textures_[id], id);
}

void Textures::setAddressMode(uint32_t id, VkSamplerAddressMode addressMode) {
    auto device = Renderer::instance().framework()->device();

    std::scoped_lock lck(mtx_, Renderer::instance().framework()->recreateMtx());

    auto samplerIter = samplers.find(id);
    if (samplerIter == samplers.end()) {
        texturesCerr() << "The given texture id: " << id << " is not allocated for sampler" << std::endl;
        exit(EXIT_FAILURE);
    }
    if (samplers[id]->vkAddressMode() != addressMode) {
        VkFilter samplingMode = samplers[id]->vkSamplingMode();
        VkSamplerMipmapMode mipmapMode = samplers[id]->vkMipmapMode();

        auto framework = Renderer::instance().framework();
        framework->frameResourceRetainer().retain(samplers[id]);
        samplers[id] = vk::Sampler::create(device, samplingMode, mipmapMode, addressMode);
    }

    Renderer::instance().framework()->pipeline()->bindTexture(samplers[id], textures_[id], id);
}

void Textures::queueUpload(uint8_t *srcPointer,
                           uint32_t srcSizeInBytes,
                           uint32_t srcRowPixels,
                           uint32_t dstId,
                           int srcOffsetX,
                           int srcOffsetY,
                           int dstOffsetX,
                           int dstOffsetY,
                           uint32_t width,
                           uint32_t height,
                           uint32_t level) {
    std::scoped_lock lck(mtx_, Renderer::instance().framework()->recreateMtx());

    texturesCerr() << "queueUpload dstId=" << dstId << " bytes=" << srcSizeInBytes << " " << width << "x" << height
                   << " lvl=" << level << " rowPixels=" << srcRowPixels << " dstOff=" << dstOffsetX << "," << dstOffsetY
                   << std::endl;

    auto framework = Renderer::instance().framework();

    auto device = Renderer::instance().framework()->device();
    auto vma = Renderer::instance().framework()->vma();
    auto dstTextureIter = textures_.find(dstId);
    if (dstTextureIter == textures_.end()) {
        texturesCerr() << "The dstID " << dstId << " is not registered yet!" << std::endl;
        exit(EXIT_FAILURE);
    }
    auto dstTexture = (*dstTextureIter).second;

    // MC validates every writeToTexture against its own GpuTexture extent before we ever see it, so
    // a region that overruns *our* image means textures_[dstId] is stale -- it is still the image of
    // an earlier texture that held this GL id. (prepareImage silently ignores the 52 of 56 GpuFormats
    // it cannot map, and nothing unregisters an id when MC closes a texture, so a recycled id can
    // keep pointing at the previous texture's image.) Issuing the copy anyway writes outside the
    // image allocation, which faults the GPU and takes the whole device down via TDR. Drop it
    // instead, loudly, so the mismatch is diagnosable without killing the process.
    uint32_t levelWidth = dstTexture->width() >> level;
    uint32_t levelHeight = dstTexture->height() >> level;
    if (levelWidth == 0) { levelWidth = 1; }
    if (levelHeight == 0) { levelHeight = 1; }
    if (dstOffsetX < 0 || dstOffsetY < 0 || static_cast<uint32_t>(dstOffsetX) + width > levelWidth ||
        static_cast<uint32_t>(dstOffsetY) + height > levelHeight) {
        texturesCerr() << "SKIP out-of-bounds upload: dstId=" << dstId << " level=" << level << " region=" << width
                       << "x" << height << "@" << dstOffsetX << "," << dstOffsetY << " exceeds level extent "
                       << levelWidth << "x" << levelHeight << " (image " << dstTexture->width() << "x"
                       << dstTexture->height() << ", format=" << dstTexture->vkFormat() << ")" << std::endl;
        return;
    }

    auto cacheIter = caches_.find(dstId);
    if (cacheIter == caches_.end()) {
        cacheIter = caches_
                        .emplace(std::make_pair(
                            dstId, ImageBufferCache::create(vma, device, framework->swapchain()->imageCount())))
                        .first;
    }

    auto cache = cacheIter->second;
    size_t offset = cache->append(srcPointer, srcSizeInBytes);

    auto format = dstTexture->vkFormat();
    uint32_t bytePerPixel = vk::formatToByte(format);

    VkBufferImageCopy region = {};
    region.bufferRowLength = srcRowPixels;
    region.bufferOffset = offset + srcOffsetY * srcRowPixels * bytePerPixel + srcOffsetX * bytePerPixel;
    region.imageSubresource = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .mipLevel = 0,
        .baseArrayLayer = 0,
        .layerCount = 1, // avoid VK_REMAINING_ARRAY_LAYERS to get rid of maintaince5
    };
    region.imageSubresource.mipLevel = level;
    region.imageExtent = {width, height, 1};
    region.imageOffset = {dstOffsetX, dstOffsetY, 0};

    auto dstTextureUploadQueueIter = uploadQueue_->find(dstId);
    if (dstTextureUploadQueueIter == uploadQueue_->end()) {
        dstTextureUploadQueueIter = uploadQueue_->emplace(dstId, std::vector<VkBufferImageCopy>{}).first;
    }
    dstTextureUploadQueueIter->second.emplace_back(region);

    queuedUploadBytes_ += srcSizeInBytes;
    // Also flush once enough distinct textures are queued: a resource reload uploads thousands of
    // small textures that never reach the 64MB byte threshold, so without this they all defer into a
    // single giant first-frame flush that allocates a staging buffer per texture at once and exhausts
    // memory / the allocation limit. Flushing in bounded batches keeps allocations low and lets
    // collectCompletedUploadsImpl recycle staging buffers between batches.
    if (queuedUploadBytes_ >= UPLOAD_FLUSH_THRESHOLD || uploadQueue_->size() >= UPLOAD_FLUSH_TEXTURE_COUNT) {
        flushQueuedUploadImpl();
    }
}

void Textures::performQueuedUpload() {
    std::scoped_lock lck(mtx_, Renderer::instance().framework()->recreateMtx());
    collectCompletedUploadsImpl();
    flushQueuedUploadImpl();
}

std::shared_ptr<vk::HostVisibleBuffer> Textures::acquireUploadStagingBuffer(size_t minSize) {
    auto vma = Renderer::instance().framework()->vma();
    auto device = Renderer::instance().framework()->device();

    for (auto iter = freeUploadStagingBuffers_.begin(); iter != freeUploadStagingBuffers_.end(); ++iter) {
        if ((*iter)->size() >= minSize) {
            auto buffer = *iter;
            freeUploadStagingBuffers_.erase(iter);
            return buffer;
        }
    }

    return vk::HostVisibleBuffer::create(vma, device, minSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
}

std::shared_ptr<vk::Fence> Textures::acquireUploadFence() {
    auto device = Renderer::instance().framework()->device();

    if (!freeUploadFences_.empty()) {
        auto fence = freeUploadFences_.back();
        freeUploadFences_.pop_back();
        vkResetFences(device->vkDevice(), 1, &fence->vkFence());
        return fence;
    }

    return vk::Fence::create(device);
}

void Textures::collectCompletedUploadsImpl() {
    auto device = Renderer::instance().framework()->device();
    auto &batches = submittedUploadBatches_;
    for (auto iter = batches.begin(); iter != batches.end();) {
        if (vkGetFenceStatus(device->vkDevice(), iter->fence->vkFence()) == VK_SUCCESS) {
            freeUploadCommandBuffers_.emplace_back(iter->commandBuffer);
            freeUploadFences_.emplace_back(iter->fence);
            for (auto &stagingBuffer : iter->stagingBuffers) {
                freeUploadStagingBuffers_.emplace_back(stagingBuffer);
            }
            iter = batches.erase(iter);
        } else {
            ++iter;
        }
    }
}

void Textures::flushQueuedUploadImpl() {
    if (uploadQueue_ == nullptr || uploadQueue_->empty()) {
        queuedUploadBytes_ = 0;
        return;
    }

    texturesCerr() << "[Flush] " << uploadQueue_->size() << " textures, " << queuedUploadBytes_ << " bytes"
                   << std::endl;

    auto framework = Renderer::instance().framework();
    auto device = framework->device();
    auto physicalDevice = framework->physicalDevice();
    collectCompletedUploadsImpl();
    std::shared_ptr<vk::CommandBuffer> cmdBuffer;
    if (!freeUploadCommandBuffers_.empty()) {
        cmdBuffer = freeUploadCommandBuffers_.back();
        freeUploadCommandBuffers_.pop_back();
        cmdBuffer->reset();
    } else {
        cmdBuffer = vk::CommandBuffer::create(device, framework->mainCommandPool());
    }
    auto fence = acquireUploadFence();
    cmdBuffer->begin();

    auto mainQueueIndex = physicalDevice->mainQueueIndex();

    std::vector<vk::CommandBuffer::ImageMemoryBarrier> uploadPreImageBarriers, uploadPostImageBarriers;

    for (auto &entry : *uploadQueue_) {
        auto &textureId = entry.first;
        auto textureIter = textures_.find(textureId);
        if (textureIter == textures_.end()) {
            texturesCerr() << "The textureId " << textureId << " is not registered yet!" << std::endl;
            exit(EXIT_FAILURE);
        }
        auto texture = textureIter->second;
        uploadPreImageBarriers.push_back({
            .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            .oldLayout = texture->imageLayout(),
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = mainQueueIndex,
            .dstQueueFamilyIndex = mainQueueIndex,
            .image = texture,
            .subresourceRange = vk::wholeColorSubresourceRange,
        });
        texture->imageLayout() = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

        uploadPostImageBarriers.push_back({
            .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = mainQueueIndex,
            .dstQueueFamilyIndex = mainQueueIndex,
            .image = texture,
            .subresourceRange = vk::wholeColorSubresourceRange,
        });
        texture->imageLayout() = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    cmdBuffer->barriersBufferImage({}, uploadPreImageBarriers);

    for (auto &entry : *uploadQueue_) {
        auto &textureId = entry.first;
        auto &regions = entry.second;

        auto textureIter = textures_.find(textureId);
        if (textureIter == textures_.end()) {
            texturesCerr() << "The textureId " << textureId << " is not registered yet!" << std::endl;
            exit(EXIT_FAILURE);
        }
        auto texture = textureIter->second;

        auto cacheIter = caches_.find(textureId);
        if (cacheIter == caches_.end()) { continue; }
        auto cache = cacheIter->second;

        // Last line of defence before the GPU: a region is only valid against the image that was
        // registered when it was queued, and this loop resolves textures_[textureId] fresh. Anything
        // that no longer fits would write outside the image allocation and fault the device, so drop
        // it here rather than submit it.
        std::vector<VkBufferImageCopy> safeRegions;
        safeRegions.reserve(regions.size());
        for (const auto &region : regions) {
            uint32_t levelWidth = texture->width() >> region.imageSubresource.mipLevel;
            uint32_t levelHeight = texture->height() >> region.imageSubresource.mipLevel;
            if (levelWidth == 0) { levelWidth = 1; }
            if (levelHeight == 0) { levelHeight = 1; }
            if (region.imageOffset.x < 0 || region.imageOffset.y < 0 ||
                static_cast<uint32_t>(region.imageOffset.x) + region.imageExtent.width > levelWidth ||
                static_cast<uint32_t>(region.imageOffset.y) + region.imageExtent.height > levelHeight) {
                texturesCerr() << "SKIP stale upload region: id=" << textureId
                               << " level=" << region.imageSubresource.mipLevel
                               << " region=" << region.imageExtent.width << "x" << region.imageExtent.height << "@"
                               << region.imageOffset.x << "," << region.imageOffset.y << " exceeds level extent "
                               << levelWidth << "x" << levelHeight << " (image " << texture->width() << "x"
                               << texture->height() << ")" << std::endl;
                continue;
            }
            safeRegions.push_back(region);
        }
        if (safeRegions.empty()) { continue; }

        vkCmdCopyBufferToImage(cmdBuffer->vkCommandBuffer(), cache->vkBuffer(), texture->vkImage(),
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, safeRegions.size(), safeRegions.data());

        cmdBuffer->barriersBufferImage(
            {}, {{
                    .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                    .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                    VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    .srcQueueFamilyIndex = mainQueueIndex,
                    .dstQueueFamilyIndex = mainQueueIndex,
                    .image = texture,
                    .subresourceRange = vk::wholeColorSubresourceRange,
                }});
    }

    cmdBuffer->barriersBufferImage({}, uploadPostImageBarriers);

    std::vector<std::shared_ptr<vk::HostVisibleBuffer>> stagingBuffers;
    stagingBuffers.reserve(uploadQueue_->size());
    for (auto &entry : *uploadQueue_) {
        auto cacheIter = caches_.find(entry.first);
        if (cacheIter == caches_.end()) { continue; }
        auto cache = cacheIter->second;
        cache->flush();
        auto detachedBuffer = cache->detachCurrentBuffer();
        stagingBuffers.emplace_back(detachedBuffer);
        cache->replaceCurrentBuffer(acquireUploadStagingBuffer(detachedBuffer->size()));
    }

    cmdBuffer->end();
    cmdBuffer->submitMainQueueIndividual(device, fence);

    submittedUploadBatches_.push_back({
        .fence = fence,
        .commandBuffer = cmdBuffer,
        .stagingBuffers = std::move(stagingBuffers),
    });

    framework->frameResourceRetainer().retain(uploadQueue_);
    uploadQueue_ = std::make_shared<std::map<uint32_t, std::vector<VkBufferImageCopy>>>();
    queuedUploadBytes_ = 0;
}

std::shared_ptr<vk::DeviceLocalImage> Textures::texture(uint32_t id) {
    std::scoped_lock lck(mtx_, Renderer::instance().framework()->recreateMtx());
    auto iter = textures_.find(id);
    return iter != textures_.end() ? iter->second : nullptr;
}

std::shared_ptr<vk::Sampler> Textures::sampler(uint32_t id) {
    std::scoped_lock lck(mtx_, Renderer::instance().framework()->recreateMtx());
    auto iter = samplers.find(id);
    return iter != samplers.end() ? iter->second : nullptr;
}

std::shared_ptr<Emission> Textures::emission() {
    if (!Renderer::options.collectChunkEmission) {
        return nullptr;
    }

    std::scoped_lock lck(mtx_);
    if (emission_ == nullptr) {
        emission_ = Emission::create(std::weak_ptr<Textures>(shared_from_this()));
    }
    return emission_;
}

void Textures::releaseEmission() {
    std::scoped_lock lck(mtx_);
    if (emission_ != nullptr) {
        emission_->reset();
        emission_ = nullptr;
    }
}

void Textures::bindAllTextures() {
    auto device = Renderer::instance().framework()->device();

    std::scoped_lock lck(mtx_);

    for (const auto &[id, texture] : textures_) {
        if (texture == nullptr) {
            continue; // only allocated, but not initialized yet
        }

        Renderer::instance().framework()->pipeline()->bindTexture(samplers[id], texture, id);
    }
}

ImageBufferCache::ImageBufferCache(std::shared_ptr<vk::VMA> vma, std::shared_ptr<vk::Device> device, uint32_t frameNum)
    : vma_(vma), device_(device) {
    capacities_.resize(frameNum);
    bases_.resize(frameNum);
    caches_.resize(frameNum);

    for (int i = 0; i < frameNum; i++) {
        caches_[i] = vk::HostVisibleBuffer::create(vma_, device_, BASE_SIZE, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        capacities_[i] = BASE_SIZE;
        bases_[i] = 0;
    }
}

ImageBufferCache::~ImageBufferCache() {
#ifdef DEBUG
// std::cout << "ImageBufferCache deconstructed" << std::endl;
#endif
}

size_t ImageBufferCache::append(void *src, size_t size) {
    if (bases_[current_] + size >= capacities_[current_]) {
        size_t newCapacity = capacities_[current_] * 2;

        while (newCapacity < bases_[current_] + size) { newCapacity *= 2; }

        texturesCerr() << "cache resize: current=" << current_ << " size=" << size << " base=" << bases_[current_]
                       << " cap=" << capacities_[current_] << " -> newCap=" << newCapacity << std::endl;

        auto newCache = vk::HostVisibleBuffer::create(vma_, device_, newCapacity, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);

        std::memcpy(newCache->mappedPtr(), caches_[current_]->mappedPtr(), bases_[current_]);

        auto framework = Renderer::instance().framework();
        framework->frameResourceRetainer().retain(caches_[current_]);
        caches_[current_] = newCache;
        capacities_[current_] = newCapacity;
    }

    size_t ret = bases_[current_];
    std::memcpy(static_cast<uint8_t *>(caches_[current_]->mappedPtr()) + bases_[current_], src, size);
    bases_[current_] = (bases_[current_] + size + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
    return ret;
}

void ImageBufferCache::flush() {
    caches_[current_]->flush();
}

VkBuffer &ImageBufferCache::vkBuffer() {
    return caches_[current_]->vkBuffer();
}

void ImageBufferCache::reset() {
    current_ = (current_ + 1) % caches_.size();
    bases_[current_] = 0;
}

std::shared_ptr<vk::HostVisibleBuffer> ImageBufferCache::detachCurrentBuffer() {
    auto detached = caches_[current_];
    caches_[current_] = nullptr;
    capacities_[current_] = 0;
    bases_[current_] = 0;
    return detached;
}

void ImageBufferCache::replaceCurrentBuffer(std::shared_ptr<vk::HostVisibleBuffer> buffer) {
    caches_[current_] = std::move(buffer);
    capacities_[current_] = caches_[current_]->size();
    bases_[current_] = 0;
}
