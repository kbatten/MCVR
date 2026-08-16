// Next-event estimation for handheld/dynamic point lights (torch, lantern, glowstone in hand).
//
// vanilla-pt has no NEE for local area lights -- emissive blocks light the scene only through
// path-traced hits on their emissive surfaces -- so a carried torch would otherwise never cast light.
// These point lights fill that gap: each is sampled directly with a shadow ray, exactly like the sun
// (sampleSurfaceDirectLight), and its result is added into the same directLight term so it is denoised
// alongside everything else.
//
// The shadow ray reuses the sun's shadow hit group (miss index 0). Because the sun's miss shader bakes
// its own radiance into shadowRay.radiance, we cannot read radiance here; instead we read
// shadowRay.throughput as pure visibility (1 = clear, tinted for stained glass, 0 for an opaque
// occluder -- shadow.rahit zeroes it on the opaque branch). worldUBO/skyUBO/topLevelAS/shadowRay/
// mainRay/DisneyEval/SampledSurface/WORLD_MASK/PLAYER_MASK are all in scope where this is included.

#ifndef DYNAMIC_POINT_LIGHT_GLSL
#define DYNAMIC_POINT_LIGHT_GLSL

// Tunable per shader pack (configs.json handheld_light_gain / handheld_light_range attributes). The mod
// sends the BASE light: intensity = (level/15)^2, range = level (blocks). GAIN scales brightness; RANGE
// scales the reach cap. Fallbacks match the config defaults so the shader still compiles standalone.
#ifndef MCVR_HANDHELD_LIGHT_GAIN
#define MCVR_HANDHELD_LIGHT_GAIN 5.0
#endif
#ifndef MCVR_HANDHELD_LIGHT_RANGE
#define MCVR_HANDHELD_LIGHT_RANGE 0.6
#endif
// Flame flicker toggle (config attribute handheld_light_flicker). 1 = the carried torch/lantern
// wavers over time like a flame; 0 = steady glow.
#ifndef MCVR_HANDHELD_LIGHT_FLICKER
#define MCVR_HANDHELD_LIGHT_FLICKER 1
#endif
// Flicker speed scales the flame-waver frequency; intensity scales its amplitude. Both are config
// attributes; 1.0 = the tuned baseline, 0.0 = effectively steady.
#ifndef MCVR_HANDHELD_LIGHT_FLICKER_SPEED
#define MCVR_HANDHELD_LIGHT_FLICKER_SPEED 1.0
#endif
#ifndef MCVR_HANDHELD_LIGHT_FLICKER_INTENSITY
#define MCVR_HANDHELD_LIGHT_FLICKER_INTENSITY 1.0
#endif

vec3 sampleSurfaceDynamicPointLights(SampledSurface surface, vec3 viewDir) {
    uint count = min(skyUBO.dynamicLightCount, uint(MCVR_MAX_DYNAMIC_LIGHTS));
    if (count == 0u) { return vec3(0.0); }

    bool isOpaqueSurface = surface.mat.transmission <= EPS;
    vec3 result = vec3(0.0);

    // Flame flicker: a subtle time-varying multiplier so a carried torch/lantern wavers like a real
    // flame instead of a flat glow. Sum of a few incommensurate sines (mean ~1, clamped) reads as
    // organic and non-repeating; clocked by worldUBO.flickerTime (monotonic seconds). Same for every
    // handheld light this frame, so computed once. Gated by the handheld_light_flicker config toggle.
    float flicker = 1.0;
    if (MCVR_HANDHELD_LIGHT_FLICKER != 0) {
        float ft = worldUBO.flickerTime * MCVR_HANDHELD_LIGHT_FLICKER_SPEED;
        float wave = 0.08 * sin(ft * 6.3)
                   + 0.05 * sin(ft * 13.7 + 1.7)
                   + 0.035 * sin(ft * 24.1 + 4.2);
        flicker = clamp(1.0 + wave * MCVR_HANDHELD_LIGHT_FLICKER_INTENSITY, 0.1, 2.0);
    }

    for (uint i = 0u; i < count; ++i) {
        DynamicLight light = skyUBO.dynamicLights[i];
        if (light.intensity <= 0.0) { continue; }

        vec3 toLight = light.position - surface.worldPos; // both in camera-relative scene space
        float dist2 = dot(toLight, toLight);
        float range = max(light.range * MCVR_HANDHELD_LIGHT_RANGE * 0.25, 0.5);
        if (dist2 > range * range) { continue; }

        float dist = sqrt(max(dist2, 1e-8));
        vec3 L = toLight / dist;

        float NoL = dot(L, surface.geometricNormal);
        if (isOpaqueSurface && NoL <= 0.0) { continue; }

        float lightPdf;
        vec3 brdf = DisneyEval(surface.mat, viewDir, surface.shadingNormal, L, lightPdf);
        if (lightPdf <= 1e-6) { continue; }

        // Linear-radius falloff: full brightness at the source, fading steadily to 0 at the range cap R.
        // Reads like a natural torch (brighter at your feet, dimmer at the edge) rather than a uniform
        // flood. (Was the flat-top window 1 - (d/R)^4.)
        float distNorm = clamp(dist / range, 0.0, 1.0);
        float atten = 1.0 - distNorm;

        // Visibility via a shadow ray toward the light (finite length = distance to it).
        shadowRay.radiance = vec3(0.0);
        shadowRay.throughput = vec3(1.0);
        shadowRay.insideBoat = rayInsideBoat(mainRay) ? 1u : 0u;
        shadowRay.pad0 = 0u;

        vec3 visibilityNormal = NoL >= 0.0 ? surface.geometricNormal : -surface.geometricNormal;
        vec3 shadowOrigin = surface.worldPos + visibilityNormal * 0.0002;
        float shadowLen = max(dist - 0.001, 0.0001);
        uint shadowMask = WORLD_MASK | PLAYER_MASK;
        traceRayEXT(topLevelAS, gl_RayFlagsNoneEXT, shadowMask, 0, 0, 0, shadowOrigin, 0.0001, L, shadowLen, 1);

        vec3 visibility = shadowRay.throughput; // 0 opaque-blocked, tint for glass, 1 clear
        result += visibility * mainRay.throughput * brdf * light.color
                  * light.intensity * MCVR_HANDHELD_LIGHT_GAIN * atten * flicker;
    }

    return result;
}

#endif
