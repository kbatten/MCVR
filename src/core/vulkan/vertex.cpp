#include "core/vulkan/vertex.hpp"

#include <iostream>

#include "common/shared.hpp"

uint32_t vk::Vertex::packMaterialFlags(const VertexFormat::PBRVertex &vertex) {
    uint32_t packed = 0;
    packed |= vertex.useColorLayer > 0 ? useColorLayerBit : 0u;
    packed |= vertex.useTexture > 0 ? useTextureBit : 0u;
    packed |= vertex.useOverlay > 0 ? useOverlayBit : 0u;
    packed |= vertex.useGlint > 0 ? useGlintBit : 0u;
    packed |= vertex.useNorm > 0 ? useNormBit : 0u;
    packed |= vertex.useLight > 0 ? useLightBit : 0u;
    packed |= (vertex.alphaMode & 0xFu) << alphaModeShift;
    packed |= (vertex.coordinate & 0xFu) << coordinateShift;
    return packed;
}

vk::VertexFormat::PositionVertex vk::Vertex::makePositionVertex(const VertexFormat::PBRVertex &vertex) {
    return {
        .pos = vertex.pos,
        .pad0 = 0,
    };
}

vk::VertexFormat::MaterialVertex vk::Vertex::makeMaterialVertex(const VertexFormat::PBRVertex &vertex) {
    // Black-terrain diagnostic (reliable, native -- the rgen shader probes are moot under NRD demodulation).
    // The atlas (glId 29, 4096x2048) is filled and bound at RT slot 29, textureID=29 is captured, geometry is
    // in the TLAS -- yet black. Log the first few textured vertices' textureID + textureUV to check the UV is
    // sane atlas-space [0,1] (not 0 / not pixel coords / not a wrong range). uv=0 or huge => UV capture/space
    // bug; sane => the fault is downstream (material/lighting), not albedo.
    static int radianceMatDbg = 0;
    if (radianceMatDbg < 12 && vertex.useTexture > 0) {
        radianceMatDbg++;
        std::cerr << "[MatDbg] textureID=" << vertex.textureID << " uv=(" << vertex.textureUV.x << ", "
                  << vertex.textureUV.y << ") useTex=" << vertex.useTexture << " useColor=" << vertex.useColorLayer
                  << " color=(" << vertex.colorLayer.x << "," << vertex.colorLayer.y << "," << vertex.colorLayer.z
                  << ")" << std::endl;
    }
    return {
        .norm = vertex.norm,
        .textureID = vertex.textureID,
        .colorLayer = vertex.colorLayer,
        .textureUV = vertex.textureUV,
        .overlayUV = vertex.overlayUV,
        .glintUV = vertex.glintUV,
        .glintTexture = vertex.glintTexture,
        .albedoEmission = vertex.albedoEmission,
        .lightUV = vertex.lightUV,
        .packedData = packMaterialFlags(vertex),
        .pad0 = 0,
    };
}

std::vector<vk::VertexFormat::PositionVertex>
vk::Vertex::buildPositionVertices(const std::vector<VertexFormat::PBRVertex> &vertices) {
    std::vector<VertexFormat::PositionVertex> packedVertices;
    packedVertices.reserve(vertices.size());
    for (const auto &vertex : vertices) { packedVertices.push_back(makePositionVertex(vertex)); }
    return packedVertices;
}

std::vector<vk::VertexFormat::MaterialVertex>
vk::Vertex::buildMaterialVertices(const std::vector<VertexFormat::PBRVertex> &vertices) {
    std::vector<VertexFormat::MaterialVertex> packedVertices;
    packedVertices.reserve(vertices.size());
    for (const auto &vertex : vertices) { packedVertices.push_back(makeMaterialVertex(vertex)); }
    return packedVertices;
}

template <>
vk::VertexLayoutInfo &vk::Vertex::vertexLayoutInfo<vk::VertexFormat::Triangle>() {
    static std::vector<VertexAttribute> attributes = {
        {VK_FORMAT_R32G32B32_SFLOAT, offsetof(vk::VertexFormat::Triangle, pos)},
        {VK_FORMAT_R32G32B32_SFLOAT, offsetof(vk::VertexFormat::Triangle, color)},
    };

    static vk::VertexLayoutInfo vertexLayoutInfo = initVertexLayout<vk::VertexFormat::Triangle>(attributes);
    return vertexLayoutInfo;
}

template <>
vk::VertexLayoutInfo &vk::Vertex::vertexLayoutInfo<vk::VertexFormat::TexturedTriangle>() {
    static std::vector<VertexAttribute> attributes = {
        {VK_FORMAT_R32G32B32_SFLOAT, offsetof(vk::VertexFormat::TexturedTriangle, pos)},
        {VK_FORMAT_R32G32_SFLOAT, offsetof(vk::VertexFormat::TexturedTriangle, uv)},
    };
    static vk::VertexLayoutInfo vertexLayoutInfo = initVertexLayout<vk::VertexFormat::TexturedTriangle>(attributes);
    return vertexLayoutInfo;
}

template <>
vk::VertexLayoutInfo &vk::Vertex::vertexLayoutInfo<vk::VertexFormat::ArrayTexturedTriangle>() {
    static std::vector<VertexAttribute> attributes = {
        {VK_FORMAT_R32G32B32_SFLOAT, offsetof(vk::VertexFormat::ArrayTexturedTriangle, pos)},
        {VK_FORMAT_R32G32_SFLOAT, offsetof(vk::VertexFormat::ArrayTexturedTriangle, uv)},
        {VK_FORMAT_R32_SFLOAT, offsetof(vk::VertexFormat::ArrayTexturedTriangle, textureLayer)},
    };
    static vk::VertexLayoutInfo vertexLayoutInfo = initVertexLayout<vk::VertexFormat::ArrayTexturedTriangle>(attributes);
    return vertexLayoutInfo;
}
