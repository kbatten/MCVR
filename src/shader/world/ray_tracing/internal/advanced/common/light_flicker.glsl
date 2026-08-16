#ifndef LIGHT_FLICKER_GLSL
#define LIGHT_FLICKER_GLSL

// Shared flame flicker for handheld AND placed block lights. lightFlickerFactor() returns a mean~1
// multiplier that wavers like a flame: a sum of a few incommensurate sines (organic, non-repeating),
// clamped so it never fully dims or spikes. `speed` (config slider) scales the waver frequency,
// `intensity` (config slider) its depth, so all flame flicker in the scene shares one feel; `amp`
// lets a caller soften the effect -- placed block lights pass a smaller amp than the handheld source
// so the scene-wide waver stays subtle. Clocked by worldUBO.flickerTime (a real monotonic-seconds
// clock; gameTime is a day fraction that wraps and cannot drive a flicker).
//
// The config TOGGLES that gate the callers live with the callers, not here:
// MCVR_HANDHELD_LIGHT_FLICKER (dynamic_point_light.glsl) and MCVR_PLACED_LIGHT_FLICKER (world rchits
// + the advanced area-light resolve).

#ifndef MCVR_HANDHELD_LIGHT_FLICKER_SPEED
#define MCVR_HANDHELD_LIGHT_FLICKER_SPEED 1.0
#endif
#ifndef MCVR_HANDHELD_LIGHT_FLICKER_INTENSITY
#define MCVR_HANDHELD_LIGHT_FLICKER_INTENSITY 1.0
#endif
#ifndef MCVR_PLACED_LIGHT_FLICKER
#define MCVR_PLACED_LIGHT_FLICKER 1
#endif

float lightFlickerFactor(float time, float amp) {
    float ft = time * MCVR_HANDHELD_LIGHT_FLICKER_SPEED;
    float wave = 0.08 * sin(ft * 6.3)
               + 0.05 * sin(ft * 13.7 + 1.7)
               + 0.035 * sin(ft * 24.1 + 4.2);
    return clamp(1.0 + wave * (MCVR_HANDHELD_LIGHT_FLICKER_INTENSITY * amp), 0.1, 2.0);
}

#endif
