#include "com_radiance_client_proxy_vulkan_ShaderProxy.h"

#include "core/all_extern.hpp"
#include "core/render/buffers.hpp"
#include "core/render/modules/ui_module.hpp"
#include "core/render/pipeline.hpp"
#include "core/render/render_framework.hpp"
#include "core/render/renderer.hpp"

#include <algorithm>
#include <unordered_map>

namespace {
std::string toStdString(JNIEnv *env, jstring value) {
    if (value == nullptr) return {};
    const char *chars = env->GetStringUTFChars(value, nullptr);
    std::string result(chars == nullptr ? "" : chars);
    if (chars != nullptr) { env->ReleaseStringUTFChars(value, chars); }
    return result;
}
} // namespace

JNIEXPORT jint JNICALL Java_com_radiance_client_proxy_vulkan_ShaderProxy_registerShader(
    JNIEnv *env,
    jclass,
    jstring shaderKey,
    jint vertexFormatType,
    jint drawMode,
    jint uniformSize,
    jstring vertexShaderPath,
    jstring fragmentShaderPath,
    jobjectArray defineNames,
    jobjectArray defineValues) {
    auto framework = Renderer::instance().framework();
    if (framework == nullptr) return -1;

    std::unordered_map<std::string, std::string> definitions;
    if (defineNames != nullptr && defineValues != nullptr) {
        jsize count = std::min(env->GetArrayLength(defineNames), env->GetArrayLength(defineValues));
        for (jsize i = 0; i < count; ++i) {
            auto name = static_cast<jstring>(env->GetObjectArrayElement(defineNames, i));
            auto value = static_cast<jstring>(env->GetObjectArrayElement(defineValues, i));
            definitions.emplace(toStdString(env, name), toStdString(env, value));
            env->DeleteLocalRef(name);
            env->DeleteLocalRef(value);
        }
    }

    auto shaderId = framework->pipeline()->uiModule()->registerOverlayDrawShader(
        toStdString(env, shaderKey), vertexFormatType, drawMode, uniformSize,
        toStdString(env, vertexShaderPath), toStdString(env, fragmentShaderPath), definitions);
    return static_cast<jint>(shaderId);
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_ShaderProxy_draw(
    JNIEnv *, jclass, jint vertexId, jint indexId, jint shaderId, jint indexCount, jint indexType, jlong uniformPtr,
    jint uniformSize) {
    auto framework = Renderer::instance().framework();
    if (framework == nullptr) return;
    // registerShader returns -1 for a shader it could not build; drawing with that would index the
    // shader table out of range. The Java side already skips these draws -- this is the backstop.
    if (shaderId < 0) return;
    auto vertexBuffer = Renderer::instance().buffers()->getBuffer(vertexId);
    auto indexBuffer = Renderer::instance().buffers()->getBuffer(indexId);
    uint32_t uniformOffset = 0;
    Renderer::instance().buffers()->appendOverlayDrawUniform(
        reinterpret_cast<uint8_t *>(uniformPtr), uniformSize, uniformOffset);
    auto context = framework->safeAcquireCurrentContext();
    auto pipelineContext = framework->pipeline()->acquirePipelineContext(context);
    pipelineContext->uiModuleContext->drawIndexed(vertexBuffer, indexBuffer, shaderId, uniformOffset, indexCount,
                                                  static_cast<VkIndexType>(indexType));
}
