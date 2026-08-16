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

// [PROBE #23] Reach visualizer: within the light's range, force full brightness with NO falloff, NO
// shadow ray, and NO NoL cull. If the room then fills with light out to the range cap, the range/loop
// are fine and the normal path's falloff/shadow/NoL was the limiter. If it STILL only reaches ~2
// blocks, the range value isn't reaching the shader (UBO packing) or the loop isn't evaluating distant
// surfaces. Set to 0 / strip once diagnosed.
#define RADIANCE_PROBE_HANDHELD_REACH 1

vec3 sampleSurfaceDynamicPointLights(SampledSurface surface, vec3 viewDir) {
    uint count = min(skyUBO.dynamicLightCount, uint(MCVR_MAX_DYNAMIC_LIGHTS));
    if (count == 0u) { return vec3(0.0); }

    bool isOpaqueSurface = surface.mat.transmission <= EPS;
    vec3 result = vec3(0.0);

    for (uint i = 0u; i < count; ++i) {
        DynamicLight light = skyUBO.dynamicLights[i];
        if (light.intensity <= 0.0) { continue; }

        vec3 toLight = light.position - surface.worldPos; // both in camera-relative scene space
        float dist2 = dot(toLight, toLight);
        float range = max(light.range, 0.5);
        if (dist2 > range * range) { continue; }
#if RADIANCE_PROBE_HANDHELD_REACH
        // Encode the shader-side range value as COLOR on the lit surfaces, to pin the bug:
        //   BLUE  = light.range >= 30  (== the 35 Java sends -> value is read correctly)
        //   RED   = light.range <  30  (misread -> UBO/std140 layout mismatch)
        // Then read the EXTENT of the glow together with the color:
        //   RED + ~2 blocks   -> range value is wrong (UBO layout).
        //   BLUE + ~2 blocks  -> range is 35 but dist2 is too big: light.position vs surface.worldPos
        //                        are in different spaces/scales (the cutoff dist2 > range*range fires
        //                        early).  BLUE + far -> range & distance both fine.
        result += (light.range >= 30.0) ? vec3(0.0, 0.0, 2.0) : vec3(2.0, 0.0, 0.0);
        continue;
#endif

        float dist = sqrt(max(dist2, 1e-8));
        vec3 L = toLight / dist;

        float NoL = dot(L, surface.geometricNormal);
        if (isOpaqueSurface && NoL <= 0.0) { continue; }

        float lightPdf;
        vec3 brdf = DisneyEval(surface.mat, viewDir, surface.shadingNormal, L, lightPdf);
        if (lightPdf <= 1e-6) { continue; }

        // Handheld lights need vanilla-torch REACH (fill a room). Placed torches light rooms through
        // emissive GI (x16 indirect boost), which a x1 direct point light can't match by intensity
        // alone -- so keep the near-field where it is (brightness is fine) and instead flatten the
        // falloff to stay near full through the room, only diving to 0 near the range cap R. Window
        // 1 - (d/R)^4: ~1 out to ~0.7R, then eases off. (Parabolic 1-(d/R)^2 still halved by ~0.7R.)
        float distNorm = clamp(dist / range, 0.0, 1.0);
        float distNorm2 = distNorm * distNorm;
        float atten = 1.0 - distNorm2 * distNorm2;

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
        result += visibility * mainRay.throughput * brdf * light.color * light.intensity * atten;
    }

    return result;
}

#endif
