/*
 * Copyright 2021 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#include "BlurFilter.h"
#include <SkBlendMode.h>
#include <cmath>
#include <SkM44.h>
#include <SkCanvas.h>
#include <SkPaint.h>
#include <SkRRect.h>
#include <SkRuntimeEffect.h>
#include <SkSize.h>
#include <SkString.h>
#include <SkSurface.h>
#include <SkTileMode.h>
#include <common/ThreadStateCrashLogger.h>
#include <common/trace.h>
#include <log/log.h>
#include <sys/system_properties.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

#include "RuntimeEffectManager.h"

namespace android {
namespace renderengine {
namespace skia {

const SkString kEffectSource_BlurFilter_MixEffect(R"(
    uniform shader blurredInput;
    uniform shader originalInput;
    uniform float mixFactor;

    half4 main(float2 xy) {
        return half4(mix(originalInput.eval(xy), blurredInput.eval(xy), mixFactor)).rgb1;
    }
)");

// Glass slab rim: flat across the middle, rolling off along a squircle h(x) = (1 - (1 - x)^4)^(1/4),
// x being the depth into the rim. Snell's law bends the view outwards along it.
// lens: x = rim depth, y = shift at the very edge, z = refractive index, w = the largest
// tan(incident - refracted), which scales the shift to y. lensHeight: rim height / rim depth.
// lensRim: x = width, y = strength of the light caught along the rim (dock material only).
#define LENS_SKSL                                                                   \
    "uniform float4 lens;\n"                                                       \
    "uniform float lensHeight;\n"                                                  \
    "uniform float2 lensRim;\n"                                                   \
    "float2 lensNormal(float2 rel, float2 h, float r) {\n"                         \
    "    float2 q = abs(rel) - (h - r);\n"                                         \
    "    float2 n = (q.x > 0.0 && q.y > 0.0) ? normalize(q)\n"                     \
    "             : (q.x > q.y ? float2(1.0, 0.0) : float2(0.0, 1.0));\n"          \
    "    return n * float2(rel.x < 0.0 ? -1.0 : 1.0, rel.y < 0.0 ? -1.0 : 1.0);\n" \
    "}\n"                                                                          \
    "float lensShift(float depth) {\n"                                             \
    "    float k = 1.0 - clamp(depth / lens.x, 0.0, 1.0);\n"                       \
    "    if (k <= 0.0 || lens.y <= 0.0) return 0.0;\n"                             \
    "    float k3 = k * k * k;\n"                                                  \
    "    float slope = lensHeight * k3 / pow(max(1.0 - k3 * k, 1e-4), 0.75);\n"    \
    "    float incident = atan(slope);\n"                                          \
    "    float refracted = asin(sin(incident) / lens.z);\n"                        \
    "    return lens.y * tan(incident - refracted) / lens.w;\n"                    \
    "}\n"

// Evolution Liquid Glass: the rim refracts, the colours behind are lifted and the rim catches light
const SkString kEffectSource_LiquidGlassEffect(LENS_SKSL R"(
    uniform shader blurredInput;
    uniform shader originalInput;
    uniform float clarity;
    uniform float4 bounds;
    uniform float radius;
    uniform float band;
    uniform float rim;
    uniform float saturation;
    uniform float gain;
    uniform float lift;
    uniform float edgeClear;

    float roundRect(float2 p, float2 c, float2 h, float r) {
        float2 q = abs(p - c) - (h - r);
        return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
    }

    half4 main(float2 xy) {
        float2 c = (bounds.xy + bounds.zw) * 0.5;
        float2 h = (bounds.zw - bounds.xy) * 0.5;
        float r = min(radius, min(h.x, h.y));
        float d = roundRect(xy, c, h, r);
        // The outward normal straight from the distance field's own terms, rather than by four
        // more evaluations of it per pixel.
        float2 n = lensNormal(xy - c, h, r);

        // 0 across the flat middle, 1 at the very edge.
        float t = clamp(1.0 + d / band, 0.0, 1.0);
        float2 src = xy + n * lensShift(-d);
        // Frosted across the middle so what sits on the glass stays legible, clearer towards the
        // rim, where the bent picture of what's behind shows through.
        float clear = clarity + (1.0 - clarity) * edgeClear * t * t;
        half3 rgb = blurredInput.eval(src).rgb;
        // Most of a slab is its flat middle, where nothing of the sharp backdrop shows.
        if (clear > 0.002) {
            rgb = mix(rgb, originalInput.eval(src).rgb, half(clear));
        }

        half l = dot(rgb, half3(0.2126, 0.7152, 0.0722));
        rgb = mix(half3(l), rgb, half(saturation));
        rgb = rgb * half(gain) + half(lift);

        float facing = dot(n, normalize(float2(-0.6, -1.0)));
        float edge = pow(t, 6.0);
        rgb += half(rim * edge * (0.55 + 0.45 * facing));
        rgb += half(rim * 0.35 * edge * max(-facing, 0.0));
        return half4(clamp(rgb, 0.0, 1.0), 1.0);
    }
)");

const SkString kEffectSource_CosGlassEffect(LENS_SKSL R"(
    uniform shader blurredInput;
    uniform half4 modes;
    uniform half4 color0;
    uniform half4 color1;
    uniform half4 color2;
    uniform half4 color3;
    uniform half4 lift;
    uniform float4 bounds;
    uniform float radius;
    uniform float edge[16];
    uniform float glow[9];

    float rbox(float2 p, float2 b, float r) {
        float2 q = abs(p) - b + r;
        return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
    }
    float getDis(float2 pos, float2 halfSize) {
        float2 rect = abs(pos - halfSize);
        return rect.x + rect.y;
    }
    float random(float2 st) {
        return fract(sin(dot(st, float2(12.9898, 78.233))) * 43758.5453);
    }
    float softLight(float s, float d) {
        return s <= 0.5 ? d - (1.0 - 2.0 * s) * d * (1.0 - d) : d + (2.0 * s - 1.0) * (sqrt(d) - d);
    }
    half3 innerGlow(float2 xy, float2 pos, float2 halfSize, float corner, float d, half3 color) {
        float gray = clamp((dot(float3(0.2126, 0.7152, 0.0722), color) - 0.6) * 1.6, 0.0, 1.0);
        float2 fade = float2(glow[0], glow[1]) * (1.0 - gray) * (1.0 - gray);
        if (fade.x < 1e-4 && fade.y < 1e-4) {
            return color;
        }
        float fadeLine = -glow[2];
        float2 halfFade = halfSize * float2(glow[5], glow[6]);
        float fadeCorner = min(min(halfFade.x, halfFade.y), max(glow[2] * 1.2, corner));
        float fadeSdf = rbox(pos - float2(glow[3], glow[4]), halfFade, fadeCorner);
        float alpha = smoothstep(fadeLine, 0.0, fadeSdf) * smoothstep(fadeLine, 0.0, d);
        if (glow[8] > 1e-4) {
            alpha = pow(alpha, glow[7]) * glow[8] + (1.0 - glow[8]) * alpha;
        }
        alpha = alpha * (fade.x - fade.y) + fade.y;
        float n = clamp((random(xy * 0.013) - 0.5) * 0.008 + 0.5, 0.0, 1.0);
        alpha = clamp(softLight(n, alpha), 0.0, 1.0);
        return saturate(mix(color, color + 1.0, alpha));
    }
    half3 rimLight(float2 pos, float2 halfSize, float d, half3 color, half3 raw) {
        float2 lineNear = float2(edge[3], edge[4]);
        float2 lineFar = float2(edge[5], edge[6]);
        if (d < -max(lineNear.y, lineFar.y)) {
            return color;
        }
        float distanceAll = (halfSize.x + halfSize.y) * 2.0;
        float ratio = mod(edge[13] + halfSize.x / distanceAll * 0.5 + 0.5, 1.0);
        float2 dirAbs = abs(normalize(pos));
        float2 posN = min(float2(dirAbs.xy * halfSize.yx / max(float2(0.001), dirAbs.yx)), halfSize)
                * sign(pos);
        float flag = mix(-1.0, 1.0, step(ratio, 0.5)) * dot(float2(-halfSize.y, halfSize.x), posN);
        float disPoint = distanceAll * min(ratio, 1.0 - ratio) * 2.0;
        float disPos = getDis(posN, halfSize);
        float near = mix(disPoint + disPos, abs(disPoint - disPos), step(0.0, flag));
        near = min(2.0 * distanceAll - near, near);
        float nearAlpha = edge[7] * smoothstep(edge[10] * distanceAll, edge[9] * distanceAll, near);
        float farAlpha =
                edge[8] * smoothstep(edge[12] * distanceAll, edge[11] * distanceAll, distanceAll - near);
        float nearSdf = smoothstep(-lineNear.y, -lineNear.x, d);
        float farSdf = smoothstep(-lineFar.y, -lineFar.x, d);
        if (edge[15] > 1e-4) {
            nearSdf = pow(nearSdf, edge[14]) * edge[15] + (1.0 - edge[15]) * nearSdf;
            farSdf = pow(farSdf, edge[14]) * edge[15] + (1.0 - edge[15]) * farSdf;
        }
        float lineAlpha = clamp(nearAlpha * nearSdf + farAlpha * farSdf, 0.0, 1.0);
        float gray = clamp(dot(float3(0.2126, 0.7152, 0.0722), raw) * 1.2, 0.0, 1.0);
        float improve = (1.0 - gray) * (1.0 - gray);
        improve = improve * improve * 0.1;
        half3 lit = saturate(raw + improve);
        half3 lineColor = half3(edge[0], edge[1], edge[2]);
        lit = mix(2.0 * lit * lineColor, 1.0 - 2.0 * (1.0 - lit) * (1.0 - lineColor),
                  step(lineColor, half3(0.5)));
        return mix(color, saturate(lit), lineAlpha);
    }

    half overlay(half s, half d) {
        return (d < 0.5) ? 2.0 * s * d : 1.0 - 2.0 * (1.0 - s) * (1.0 - d);
    }
    half3 overlay(half3 s, half3 d) {
        return half3(overlay(s.x, d.x), overlay(s.y, d.y), overlay(s.z, d.z));
    }
    half colorDodge(half s, half d) { return d / (1.0 - s); }
    half3 colorDodge(half3 s, half3 d) {
        return half3(colorDodge(s.x, d.x), colorDodge(s.y, d.y), colorDodge(s.z, d.z));
    }
    half3 luminosity(half3 s, half3 d) {
        half dLum = dot(d, half3(0.3, 0.59, 0.11));
        half sLum = dot(s, half3(0.3, 0.59, 0.11));
        half3 c = d + (sLum - dLum);
        half minC = min(min(c.x, c.y), c.z);
        half maxC = max(max(c.x, c.y), c.z);
        if (minC < 0.0) {
            return sLum + ((c - sLum) * sLum) / (sLum - minC);
        } else if (maxC > 1.0) {
            return sLum + ((c - sLum) * (1.0 - sLum)) / (maxC - sLum);
        }
        return c;
    }
    half3 layer(half3 col, half mode, half4 c) {
        if (mode == 1) {
            col = mix(col, c.rgb, c.a);
        } else if (mode == 2) {
            col = mix(col, overlay(c.rgb, col), c.a);
        } else if (mode == 3) {
            col = mix(col, colorDodge(c.rgb, col), c.a);
        } else if (mode == 4) {
            col = mix(col, luminosity(c.rgb, col), c.a);
            col = mix(col, overlay(c.rgb, col), c.a);
        } else if (mode == 5) {
            col = mix(col, luminosity(c.rgb, col), c.a);
            col = mix(col, colorDodge(c.rgb, col), c.a);
        } else if (mode == 7) {
            col = mix(col, luminosity(c.rgb, col), c.a);
        } else if (mode == 6) {
            // Neutral glass retains dark/saturated content instead of washing it with grey.
            half luminance = dot(col, half3(0.2126, 0.7152, 0.0722));
            col *= mix(half3(1), c.rgb, c.a * smoothstep(0.2, 1.0, luminance));
        }
        return col;
    }
    half4 main(float2 xy) {
        float2 halfSize = (bounds.zw - bounds.xy) * 0.5;
        float2 pos = xy - (bounds.xy + halfSize);
        float corner = min(radius, min(halfSize.x, halfSize.y));
        float d = rbox(pos, halfSize, corner);
        float2 src = xy;
        float2 n = float2(0.0);
        // The dock's material, in the iOS style, refracts what lies beyond its rim.
        if (lens.y > 0.0 || lensRim.y > 0.0) {
            n = lensNormal(pos, halfSize, corner);
            src += n * lensShift(-d);
        }
        half4 col = blurredInput.eval(src);
        half3 rgb = layer(col.rgb, modes.x, color0);
        rgb = layer(rgb, modes.y, color1);
        rgb = layer(rgb, modes.z, color2);
        rgb = layer(rgb, modes.w, color3);
        rgb = rgb * lift.x + lift.y;
        half luma = dot(rgb, half3(0.2126, 0.7152, 0.0722));
        rgb = saturate(mix(half3(luma), rgb, lift.z));
        if (edge[7] + edge[8] > 0.0 || glow[0] > 0.0) {
            rgb = innerGlow(xy, pos, halfSize, corner, d, rgb);
            rgb = rimLight(pos, halfSize, d, rgb, col.rgb);
        }
        if (lensRim.y > 0.0) {
            // Light from the top left catches the rim where it faces it and bounces faintly off
            // the far side; the edges turned across it stay dim.
            float e = 1.0 - smoothstep(0.0, lensRim.x, -d);
            float facing = dot(n, float2(-0.5145, -0.8575));
            float spec = pow(max(facing, 0.0), 2.0) + 0.45 * pow(max(-facing, 0.0), 2.0);
            rgb = saturate(rgb + half(lensRim.y * e * (0.15 + 0.85 * spec)));
        }
        return half4(rgb, 1.0);
    }
)");

static constexpr const char* kLiquidGlassProperty = "persist.sys.evolution.liquid_glass";
// The names of the screens running a DeX desktop, separated by '|', which have Liquid Glass even
// with the system switch off.
static constexpr const char* kDexGlassProperty = "sys.evolution.liquid_glass.dex";
static constexpr const char* kLiquidGlassStyleProperty = "persist.sys.evolution.liquid_glass_style";

// A system property's value, re-read only when it changes.
class CachedProperty {
public:
    explicit CachedProperty(const char* name) : mName(name) {}

    const std::string& get() {
        if (mInfo == nullptr) {
            const uint32_t area = __system_property_area_serial();
            if (area == mAreaSerial) return mValue;
            mAreaSerial = area;
            mInfo = __system_property_find(mName);
            if (mInfo == nullptr) return mValue;
        }
        const uint32_t serial = __system_property_serial(mInfo);
        if (serial != mSerial) {
            mSerial = serial;
            __system_property_read_callback(
                    mInfo,
                    [](void* cookie, const char*, const char* value, uint32_t) {
                        *static_cast<std::string*>(cookie) = value;
                    },
                    &mValue);
        }
        return mValue;
    }

private:
    const char* mName;
    const prop_info* mInfo = nullptr;
    uint32_t mAreaSerial = UINT32_MAX;
    uint32_t mSerial = UINT32_MAX;
    std::string mValue;
};

static bool cosGlassStyle() {
    static CachedProperty style(kLiquidGlassStyleProperty);
    return style.get() == "cos";
}

// Whether Penguin Laboratory's Liquid Glass is on, or the display is one of DeX's desktops, where
// it always is. namePlusId is "<display name> (<id>)", after "ScreenCapture, " for screenshots.
static bool liquidGlassEnabledFor(const std::string& namePlusId) {
    static CachedProperty global(kLiquidGlassProperty);
    static CachedProperty dex(kDexGlassProperty);
    const std::string& on = global.get();
    if (on == "1" || on == "true") return true;
    const std::string& names = dex.get();
    size_t start = 0;
    while (start < names.size()) {
        size_t end = names.find('|', start);
        if (end == std::string::npos) end = names.size();
        if (end > start) {
            // Compositing names the display alone; screenshots put "ScreenCapture, " first.
            const std::string name = names.substr(start, end - start) + " (";
            if (namePlusId.compare(0, name.size(), name) == 0 ||
                namePlusId.find(", " + name) != std::string::npos) {
                return true;
            }
        }
        start = end + 1;
    }
    return false;
}

// A Liquid Glass value overridden live from debug.evolution.lg.sf.<name>, for matching the material by eye.
static float tune(const char* name, float fallback) {
    static std::mutex lock;
    static std::map<std::string, std::unique_ptr<CachedProperty>> props;
    std::lock_guard<std::mutex> guard(lock);
    auto& prop = props[name];
    if (prop == nullptr) {
        prop = std::make_unique<CachedProperty>(
                strdup((std::string("debug.evolution.lg.sf.") + name).c_str()));
    }
    const std::string& value = prop->get();
    if (value.empty()) return fallback;
    char* end = nullptr;
    const float parsed = strtof(value.c_str(), &end);
    return end != value.c_str() && std::isfinite(parsed) ? parsed : fallback;
}

// The same, for one material: debug.evolution.lg.sf.m<material>_<key>.
static float tuneMaterial(int32_t material, const char* key, float fallback) {
    char name[32];
    snprintf(name, sizeof(name), "m%d_%s", material, key);
    return tune(name, fallback);
}

// What one material's glass is made of, once its tunables are resolved: the scale of its colour
// layers' alphas, the lift (rgb * gain + add, then saturation), and the rim and glow multipliers.
struct MaterialRecipe {
    float layerScale = 1.0f;
    // The light card's layer alphas from its older properties, NaN while unset.
    float layer0Alpha = NAN;
    float layer1Alpha = NAN;
    float gain = 1.0f;
    float add = 0.0f;
    float sat = 1.0f;
    float rim = 1.0f;
    float glow = 1.0f;
    float rimPx = 0.875f;
};

struct GlassTunables {
    // Every material without Liquid Glass, and those it doesn't tune, which keep the cos_* props.
    MaterialRecipe plain;
    MaterialRecipe glass[15];
};

static bool tunedGlassMaterial(int32_t material) {
    return (material >= 1 && material <= 10) || material == 13 || material == 14;
}

// The panels, 5 and 6, draw no rim or glow.
static bool glassHasRim(int32_t material) {
    return (material >= 1 && material <= 4) || (material >= 7 && material <= 10) ||
            material == 13 || material == 14;
}

// The dialogs, 13 and 14, have no colour layers for a layer scale to act on.
static bool glassHasLayers(int32_t material) {
    return material != 13 && material != 14;
}

// A rim or glow multiplier's product with the global one stays finite for the shader.
static constexpr float kMaxRimGlowScale = 100.0f;

static float rimGlowScale(float perMaterial, float global) {
    return std::clamp(std::max(perMaterial, 0.0f) * global, -kMaxRimGlowScale, kMaxRimGlowScale);
}

struct GlassDefaults {
    float layers;
    float gain;
    float add;
    float sat;
};

// The materials' values under Liquid Glass. The light ones leave their colour layers off and lift
// the backdrop instead, so the colours behind them stay.
static GlassDefaults glassDefaults(int32_t material) {
    switch (material) {
        case 1:
            return {1.0f, 1.0f, 0.0f, 1.4f};
        case 3:
            return {0.0f, 1.0f, 0.06f, 1.2f};
        case 6:
            return {0.0f, 0.70f, 0.0f, 1.5f};
        case 7:
            return {1.0f, 0.6f, 0.0f, 1.5f};
        case 8:
            return {0.0f, 0.90f, 0.24f, 1.3f};
        case 13:
            return {0.0f, 0.50f, 0.07f, 1.5f};
        case 14:
            return {0.0f, 0.42f, 0.55f, 1.6f};
        default:
            return {1.0f, 1.0f, 0.0f, 1.0f};
    }
}

// Per value, m<material>_<key> wins over the older property (card_* for 7, light_card_* for 8,
// cos_* for the rest), which wins over the default. Unset reads come back as NaN.
static MaterialRecipe resolveGlassRecipe(int32_t material, float legacyGain, float legacyAdd,
                                         float legacySat, float cosRim, float cosGlow,
                                         float rimPx) {
    const GlassDefaults d = glassDefaults(material);
    MaterialRecipe r;
    if (glassHasLayers(material)) {
        r.layerScale = std::clamp(tuneMaterial(material, "layers", d.layers), 0.0f, 1.0f);
    }
    r.gain = std::clamp(tuneMaterial(material, "gain",
                                     std::isfinite(legacyGain) ? legacyGain : d.gain),
                        0.0f, 4.0f);
    r.add = std::clamp(tuneMaterial(material, "add", std::isfinite(legacyAdd) ? legacyAdd : d.add),
                       -1.0f, 1.0f);
    r.sat = std::clamp(tuneMaterial(material, "sat", std::isfinite(legacySat) ? legacySat : d.sat),
                       0.0f, 4.0f);
    if (glassHasRim(material)) {
        r.rim = rimGlowScale(tuneMaterial(material, "rim", 1.0f), cosRim);
        r.glow = rimGlowScale(tuneMaterial(material, "glow", 1.0f), cosGlow);
    }
    r.rimPx = rimPx;
    return r;
}

static void refreshGlassTunables(GlassTunables& table) {
    const float cosRim = tune("cos_rim", 1.0f);
    const float cosGlow = tune("cos_glow", 1.0f);
    const float rimPx = tune("cos_rim_px", 0.875f);
    const float cosGain = tune("cos_gain", NAN);
    const float cosAdd = tune("cos_add", NAN);
    const float cosSat = tune("cos_sat", NAN);
    const float cardGain = tune("card_gain", NAN);
    const float cardAdd = tune("card_add", NAN);
    const float cardSat = tune("card_sat", NAN);
    const float lightGain = tune("light_card_gain", NAN);
    const float lightAdd = tune("light_card_add", NAN);
    const float lightSat = tune("light_card_sat", NAN);
    const float layer0 = tune("light_card_layer0_a", NAN);
    const float layer1 = tune("light_card_layer1_a", NAN);

    table.plain = MaterialRecipe{};
    table.plain.gain = std::isfinite(cosGain) ? cosGain : 1.0f;
    table.plain.add = std::isfinite(cosAdd) ? cosAdd : 0.0f;
    table.plain.sat = std::isfinite(cosSat) ? cosSat : 1.0f;
    table.plain.rim = cosRim;
    table.plain.glow = cosGlow;
    table.plain.rimPx = rimPx;

    for (int32_t material = 1; material < 15; material++) {
        if (!tunedGlassMaterial(material)) continue;
        if (material == 7) {
            table.glass[material] = resolveGlassRecipe(material, cardGain, cardAdd, cardSat,
                                                       cosRim, cosGlow, rimPx);
        } else if (material == 8) {
            table.glass[material] = resolveGlassRecipe(material, lightGain, lightAdd, lightSat,
                                                       cosRim, cosGlow, rimPx);
            if (std::isfinite(layer0)) {
                table.glass[material].layer0Alpha = std::clamp(layer0, 0.0f, 1.0f);
            }
            if (std::isfinite(layer1)) {
                table.glass[material].layer1Alpha = std::clamp(layer1, 0.0f, 1.0f);
            }
        } else {
            table.glass[material] = resolveGlassRecipe(material, cosGain, cosAdd, cosSat, cosRim,
                                                       cosGlow, rimPx);
        }
    }
}

// The material's resolved recipe. Drawing runs per region, so the properties are read at most once
// per 500 ms, not per draw. Without Liquid Glass every material keeps the cos_* props alone.
static MaterialRecipe materialRecipe(int32_t material, bool liquidGlass) {
    static std::mutex lock;
    static GlassTunables table;
    static bool loaded = false;
    static std::chrono::steady_clock::time_point refreshed;
    std::lock_guard<std::mutex> guard(lock);
    const auto now = std::chrono::steady_clock::now();
    if (!loaded || now - refreshed >= std::chrono::milliseconds(500)) {
        refreshGlassTunables(table);
        loaded = true;
        refreshed = now;
    }
    return liquidGlass && tunedGlassMaterial(material) ? table.glass[material] : table.plain;
}

void BlurFilter::setDisplay(const std::string& namePlusId) {
    mLiquidGlass = mLiquidGlassEffect != nullptr && liquidGlassEnabledFor(namePlusId);
}

// The rim lens of a glass slab (see LENS_SKSL), in the region's own coordinates. A zero shift
// leaves what's behind the glass where it is.
struct GlassLens {
    float depth = 1.0f;
    float shift = 0.0f;
    float ior = 1.5f;
    float scale = 1.0f;
    float height = 1.0f;
    float rimWidth = 0.0f;
    float rimStrength = 0.0f;
};

// How far glass rims refract, from the Liquid Glass Refraction setting: 0-100% of the
// strongest bend, 60% by default. debug.lg.sf.lens overrides it while tuning.
static float refractionStrength() {
    static CachedProperty percent("persist.sys.evolution.liquid_glass_refraction");
    const std::string& value = percent.get();
    const int amount = std::clamp(value.empty() ? 60 : std::atoi(value.c_str()), 0, 100);
    return std::max(tune("lens", amount * 0.015f), 0.0f);
}

// One dp in pixels, for the rim's width.
static float densityScale() {
    static const float scale = [] {
        char value[PROP_VALUE_MAX] = {};
        const int density =
                __system_property_get("ro.sf.lcd_density", value) > 0 ? std::atoi(value) : 0;
        return density > 0 ? density / 160.0f : 3.0f;
    }();
    return scale;
}

// In the iOS style, the dock's material (11) is glass like iPadOS's dock and Search field, and
// so is every other rounded slab outside COS's own materials. ColorOS's glass is frosted only.
static bool iosGlass(const SkRRect& region, int32_t material, bool liquidGlass) {
    if (region.isRect() || !liquidGlass || cosGlassStyle()) return false;
    return material == 11 || material == 0;
}

static GlassLens glassLens(const SkRRect& region, int32_t material, bool liquidGlass) {
    GlassLens lens;
    if (!iosGlass(region, material, liquidGlass)) return lens;
    const SkRect& rect = region.rect();
    const float corner = region.radii(SkRRect::kUpperLeft_Corner).fX;
    const float half = std::min(rect.width(), rect.height()) * 0.5f;
    if (half < 1.0f) return lens;
    // The rim curves away over the corner's depth, never past the middle of the slab.
    lens.depth = std::clamp(corner * tune("lens_band", 1.0f), std::min(6.0f, half), half);
    lens.shift = lens.depth * refractionStrength();
    lens.ior = std::max(tune("lens_ior", 1.5f), 1.01f);
    lens.height = std::max(tune("lens_height", 1.0f), 0.01f);
    // At the very edge the surface stands upright: tan(90deg - critical angle).
    lens.scale = std::tan(static_cast<float>(M_PI_2) - std::asin(1.0f / lens.ior));
    if (material == 11) {
        lens.rimWidth = tune("lens_rim_dp", 1.5f) * densityScale();
        lens.rimStrength = std::max(tune("lens_rim", 0.35f), 0.0f);
    }
    return lens;
}

static void setLensUniforms(SkRuntimeShaderBuilder& builder, const GlassLens& lens) {
    builder.uniform("lens") = SkV4{lens.depth, lens.shift, lens.ior, lens.scale};
    builder.uniform("lensHeight") = lens.height;
    builder.uniform("lensRim") = SkV2{std::max(lens.rimWidth, 0.5f), lens.rimStrength};
}

float BlurFilter::lensReach(const SkRRect& region, int32_t material) const {
    return glassLens(region, material, mLiquidGlass).shift;
}

// The island's cards: the blurred backdrop takes the luminance of a dark gray, keeping its hue and
// saturation, then a light gray overlay, without a rim, glow or lens. The colours behind a card
// come through and the card's own shade sets its contrast.
static constexpr int32_t kIslandMaterial = 12;

struct CosGlassLayers {
    SkV4 modes;
    SkV4 colors[4];
};

static SkV4 argb(uint32_t c) {
    return SkV4{((c >> 16) & 0xff) / 255.0f, ((c >> 8) & 0xff) / 255.0f, (c & 0xff) / 255.0f,
                (c >> 24) / 255.0f};
}

static const CosGlassLayers& cosGlassLayers(int32_t material) {
    static const CosGlassLayers inactive{SkV4{5, 2, 5, 3},
                                         {argb(0xA6000000), argb(0x33999999), argb(0x40404040),
                                          argb(0x667B7B7B)}};
    static const CosGlassLayers active{SkV4{5, 2, 5, 3},
                                       {argb(0xA6000000), argb(0x33999999), argb(0x7DE6E6E6),
                                        argb(0x40CCCCCC)}};
    static const CosGlassLayers lightInactive{SkV4{5, 2, 5, 3},
                                              {argb(0x92000000), argb(0x66999999),
                                               argb(0x19404040), argb(0x4D737373)}};
    static const CosGlassLayers lightActive{SkV4{5, 2, 5, 3},
                                            {argb(0x92000000), argb(0x66999999), argb(0x73E6E6E6),
                                             argb(0x40CCCCCC)}};
    static const CosGlassLayers panel{SkV4{5, 2, 0, 0},
                                      {argb(0xA6000000), argb(0x33999999), argb(0),
                                       argb(0)}};
    static const CosGlassLayers lightPanel{SkV4{5, 2, 0, 0},
                                           {argb(0x92000000), argb(0x66999999), argb(0),
                                            argb(0)}};
    static const CosGlassLayers card{SkV4{5, 3, 0, 0},
                                     {argb(0x59404040), argb(0x66737373), argb(0), argb(0)}};
    static const CosGlassLayers lightCard{SkV4{5, 3, 0, 0},
                                          {argb(0x19404040), argb(0x4D737373), argb(0),
                                           argb(0)}};
    static const CosGlassLayers headsUp{SkV4{5, 3, 0, 0},
                                        {argb(0x8C595959), argb(0xFF333333), argb(0), argb(0)}};
    static const CosGlassLayers lightHeadsUp{SkV4{5, 3, 0, 0},
                                             {argb(0x80BFBFBF), argb(0xCC333333), argb(0),
                                              argb(0)}};
    // The dialogs have no colour layers, only the lift.
    static const CosGlassLayers dialog{SkV4{0, 0, 0, 0},
                                       {argb(0), argb(0), argb(0), argb(0)}};
    // Explicit opt-in dock material. Keep the regular COS materials and shade unchanged.
    static const CosGlassLayers dock{SkV4{6, 0, 0, 0},
                                     {argb(0xFFD4D4D4), argb(0), argb(0), argb(0)}};
    switch (material) {
        case 11:
            return dock;
        case 13:
        case 14:
            return dialog;
        case 2:
            return active;
        case 7:
            return card;
        case 8:
            return lightCard;
        case 9:
            return headsUp;
        case 10:
            return lightHeadsUp;
        case 5:
            return panel;
        case 6:
            return lightPanel;
        case 3:
            return lightInactive;
        case 4:
            return lightActive;
        default:
            return inactive;
    }
}

struct CosRimTemplate {
    float nearWidth;
    float farWidth;
    float alphaNear;
    float alphaFar;
    float fade[4];
    float glowAlpha;
    float glowOffset;
};

static const CosRimTemplate& cosRimTemplate(int32_t material) {
    static const CosRimTemplate inactiveDark{7, 4, 0.35f, 0.4f, {0.25f, 0.6f, 0.15f, 0.5f}, 0.07f,
                                             25};
    static const CosRimTemplate inactiveLight{7, 4, 0.25f, 0.6f, {0.25f, 0.6f, 0.15f, 0.5f},
                                              0.15f, 36};
    static const CosRimTemplate active{8, 2, 0.5f, 0.5f, {0.3f, 0.6f, 0.3f, 0.6f}, 0.15f, 36};
    static const CosRimTemplate headsUp{7, 4, 0.4f, 0.4f, {0.2f, 0.5f, 0.2f, 0.5f}, 0.07f, 25};
    static const CosRimTemplate lightHeadsUp{7, 4, 0.8f, 0.8f, {0.2f, 0.5f, 0.2f, 0.5f}, 0.15f,
                                             36};
    switch (material) {
        case 2:
        case 4:
            return active;
        case 9:
            return headsUp;
        case 10:
            return lightHeadsUp;
        case 3:
        case 8:
        case 14:
            return inactiveLight;
        case 13:
        default:
            return inactiveDark;
    }
}

static float cosRimRatio(float w, float h, float radius) {
    if (w <= 0 || h <= 0) return 0.5f;
    const float shortSide = std::min(w, h);
    const float corner = std::min(radius, shortSide / 2);
    const float side = (w * h - shortSide * shortSide) / (2 * shortSide);
    float pos = w >= h ? side + h / 2 : h / 2 - side;
    // A circle skips the corner offset. Ease it in as the shape stretches instead of applying it
    // whole a pixel off square, or a moving circle's flare jumps as its bounds round.
    const float longSide = std::max(w, h);
    const float stretch = radius >= shortSide / 2
            ? std::clamp((longSide - shortSide) / (shortSide * 0.25f), 0.0f, 1.0f)
            : 1.0f;
    pos -= corner / 2 * stretch;
    return pos * 0.5f / (w + h) + 0.5f;
}

static SkMatrix getShaderTransform(const SkCanvas* canvas, const SkRect& blurRect,
                                   const float scale, const float zoomScale) {
    // 1. Apply the blur shader matrix, which scales up the blurred surface to its real size
    auto matrix = SkMatrix::Scale(scale, scale);
    // 2. Since the blurred surface has the size of the layer, we align it with the
    // top left corner of the layer position.
    matrix.postConcat(SkMatrix::Translate(blurRect.fLeft, blurRect.fTop));
    // 3. Apply the "zoom" effect as an extra scale + translate around the center of the blur.
    if (zoomScale != 1.0f) {
        matrix.postScale(zoomScale, zoomScale);
        matrix.postTranslate(
                blurRect.width() * (1 - zoomScale) / 2.0f,
                blurRect.height() * (1 - zoomScale) / 2.0f);
    }
    // 4. Finally, apply the inverse canvas matrix. The snapshot made in the BlurFilter is in the
    // original surface orientation. The inverse matrix has to be applied to align the blur
    // surface with the current orientation/position of the canvas.
    SkMatrix drawInverse;
    if (canvas != nullptr && canvas->getTotalMatrix().invert(&drawInverse)) {
        matrix.postConcat(drawInverse);
    }
    return matrix;
}

BlurFilter::BlurFilter(RuntimeEffectManager& effectManager, const float maxCrossFadeRadius)
      : mMaxCrossFadeRadius(maxCrossFadeRadius),
        mMixEffect(effectManager.mKnownEffects[kBlurFilter_MixEffect]),
        mLiquidGlassEffect(effectManager.mKnownEffects[kLiquidGlassEffect]),
        mCosGlassEffect(effectManager.mKnownEffects[kCosGlassEffect]) {}

float BlurFilter::getMaxCrossFadeRadius() const {
    return mMaxCrossFadeRadius;
}

void BlurFilter::drawBlurRegion(SkCanvas* canvas, const SkRRect& effectRegion,
                                const uint32_t blurRadius, const float zoomScale,
                                const float blurAlpha,
                                const SkRect& blurRect, sk_sp<SkImage> blurredImage,
                                sk_sp<SkImage> input, int32_t material, bool panelTint) {
    SFTRACE_CALL();

    SkPaint paint;
    paint.setAlphaf(blurAlpha);

    auto blurMatrix = getShaderTransform(canvas, blurRect, kInverseInputScale, zoomScale);

    SkSamplingOptions linearSampling(SkFilterMode::kLinear, SkMipmapMode::kNone);
    const auto blurShader = blurredImage->makeShader(SkTileMode::kMirror, SkTileMode::kMirror,
                                                     linearSampling, &blurMatrix);

    // The dialogs' own glass needs Liquid Glass; without it they are the cards.
    if (!mLiquidGlass) {
        if (material == 13) material = 7;
        if (material == 14) material = 8;
    }

    bool windowGlass = false;
    if (material == 0 && mLiquidGlass && mCosGlassEffect != nullptr && cosGlassStyle() &&
        (panelTint || !effectRegion.isRect())) {
        windowGlass = effectRegion.isRect();
        material = windowGlass ? 5 : 1;
    }

    // The island's cards take their own glass, never a tile's or panel's. Below the crossfade
    // radius the glass fades in with the radius, as the plain blur's crossfade does.
    if (material == kIslandMaterial) {
        if (mCosGlassEffect == nullptr) {
            material = -1;
        } else {
            SkRuntimeShaderBuilder glass(mCosGlassEffect);
            glass.child("blurredInput") = blurShader;
            glass.uniform("modes") = SkV4{7, 2, 0, 0};
            glass.uniform("color0") = argb(0xB3262626);
            glass.uniform("color1") = argb(0x4DB3B3B3);
            glass.uniform("color2") = SkV4{0, 0, 0, 0};
            glass.uniform("color3") = SkV4{0, 0, 0, 0};
            glass.uniform("lift") = SkV4{1.0f, 0.0f, 1.0f, 0.0f};
            const SkRect& rect = effectRegion.rect();
            const float edge[16] = {};
            const float glow[9] = {};
            glass.uniform("bounds") = SkV4{rect.fLeft, rect.fTop, rect.fRight, rect.fBottom};
            glass.uniform("radius") = effectRegion.radii(SkRRect::kUpperLeft_Corner).fX;
            glass.uniform("edge").set(edge, 16);
            glass.uniform("glow").set(glow, 9);
            setLensUniforms(glass, GlassLens{});
            paint.setShader(glass.makeShader());
            paint.setAntiAlias(true);
            if (mMaxCrossFadeRadius > 0 && blurRadius < mMaxCrossFadeRadius) {
                paint.setAlphaf(blurAlpha * static_cast<float>(blurRadius) / mMaxCrossFadeRadius);
            }
            canvas->drawRRect(effectRegion, paint);
            return;
        }
    }

    if (material > 0 && mCosGlassEffect != nullptr) {
        SkRuntimeShaderBuilder glass(mCosGlassEffect);
        glass.child("blurredInput") = blurShader;
        const CosGlassLayers& layers = cosGlassLayers(material);
        const MaterialRecipe recipe = materialRecipe(material, mLiquidGlass);
        SkV4 colors[4] = {layers.colors[0], layers.colors[1], layers.colors[2], layers.colors[3]};
        for (SkV4& color : colors) color.w *= recipe.layerScale;
        // The light card's grey luminance pull greys the backdrop and its grey colour dodge clips
        // bright colours to white, so both stay off unless their own properties set an alpha.
        if (material == 8 && mLiquidGlass) {
            if (std::isfinite(recipe.layer0Alpha)) colors[0].w = recipe.layer0Alpha;
            if (std::isfinite(recipe.layer1Alpha)) colors[1].w = recipe.layer1Alpha;
        }
        glass.uniform("modes") = layers.modes;
        glass.uniform("color0") = colors[0];
        glass.uniform("color1") = colors[1];
        glass.uniform("color2") = colors[2];
        glass.uniform("color3") = colors[3];
        // Under Liquid Glass each material's lift (rgb * gain + add, then saturation around the
        // luminance) shapes its frost: the dark card is denser and more colourful so white text
        // stays readable over a bright wallpaper, the light ones lay white over the backdrop and
        // bring its colour back out from under it.
        glass.uniform("lift") = material == 11 ? SkV4{1.0f, 0.0f, 1.0f, 0.0f}
                                               : SkV4{recipe.gain, recipe.add, recipe.sat, 0.0f};
        const SkRect& rect = effectRegion.rect();
        const float corner = effectRegion.radii(SkRRect::kUpperLeft_Corner).fX;
        float edge[16] = {};
        float glow[9] = {};
        if (glassHasRim(material)) {
            const CosRimTemplate& rim = cosRimTemplate(material);
            const float px = recipe.rimPx;
            const float strength = recipe.rim;
            const float edgeValues[16] = {1, 1, 1,
                                          rim.nearWidth * px, rim.nearWidth * px * 2,
                                          rim.farWidth * px, rim.farWidth * px * 2,
                                          rim.alphaNear * strength, rim.alphaFar * strength,
                                          rim.fade[0], rim.fade[1], rim.fade[2], rim.fade[3],
                                          cosRimRatio(rect.width(), rect.height(), corner), 10,
                                          0.5f};
            std::copy(std::begin(edgeValues), std::end(edgeValues), edge);
            const float glowValues[9] = {rim.glowAlpha * recipe.glow, 0, 40 * px, 0,
                                         rim.glowOffset * px, 1.3f, 1.3f, 20, 0.5f};
            std::copy(std::begin(glowValues), std::end(glowValues), glow);
        }
        glass.uniform("bounds") = SkV4{rect.fLeft, rect.fTop, rect.fRight, rect.fBottom};
        glass.uniform("radius") = corner;
        glass.uniform("edge").set(edge, 16);
        glass.uniform("glow").set(glow, 9);
        setLensUniforms(glass, glassLens(effectRegion, material, mLiquidGlass));
        paint.setShader(glass.makeShader());
        paint.setAntiAlias(true);
        if (mMaxCrossFadeRadius > 0 && blurRadius < mMaxCrossFadeRadius) {
            // A material fades in with the radius, as the plain blur's crossfade does, instead of
            // tinting and lifting at full strength from the first radius and dropping at zero.
            paint.setAlphaf(blurAlpha * static_cast<float>(blurRadius) / mMaxCrossFadeRadius);
        }
        canvas->drawRRect(effectRegion, paint);
        return;
    }

    // Liquid Glass: rounded regions become glass slabs; whole-window blurs, such as the shade's,
    // keep the colours behind them vivid instead of greying them out, as iOS's materials do.
    if (mLiquidGlass && material >= 0 && (!effectRegion.isRect() || blurRadius >= mMaxCrossFadeRadius)) {
        const bool slab = !effectRegion.isRect();
        const SkRect& rect = effectRegion.rect();
        const float radius = slab ? effectRegion.radii(SkRRect::kUpperLeft_Corner).fX : 0.0f;
        const float shortSide = std::min(rect.width(), rect.height());
        // The curved rim is as deep as the corner, but never past the middle of the glass.
        const float band =
                slab ? std::clamp(radius * tune("band", 0.6f), 6.0f, shortSide * 0.5f) : 1.0f;
        SkRuntimeShaderBuilder glass(mLiquidGlassEffect);
        glass.child("blurredInput") = blurShader;
        SkMatrix inputMatrix;
        if (!canvas->getTotalMatrix().invert(&inputMatrix)) {
            ALOGE("matrix was unable to be inverted");
        }
        glass.child("originalInput") =
                input != nullptr
                        ? input->makeShader(SkTileMode::kMirror, SkTileMode::kMirror,
                                            linearSampling, inputMatrix)
                        : blurShader;
        glass.uniform("clarity") = tune("clarity", 0.0f);
        glass.uniform("bounds") = SkV4{rect.fLeft, rect.fTop, rect.fRight, rect.fBottom};
        glass.uniform("radius") = radius;
        glass.uniform("band") = band;
        setLensUniforms(glass, glassLens(effectRegion, material, mLiquidGlass));
        glass.uniform("rim") = slab ? tune("rim", 0.0f) : 0.0f;
        // A whole window's backdrop, such as the shade's, gets iOS's vivid blur.
        glass.uniform("saturation") = slab ? tune("slab_sat", 1.1f) : tune("window_sat", 0.5f);
        glass.uniform("gain") = slab ? tune("slab_gain", 0.9f) : tune("window_gain", 1.04f);
        glass.uniform("lift") = slab ? tune("slab_lift", 0.0f) : tune("window_lift", 0.02f);
        glass.uniform("edgeClear") = tune("edge_clear", 0.08f);
        paint.setShader(glass.makeShader());
        if (slab) {
            paint.setAntiAlias(true);
            canvas->drawRRect(effectRegion, paint);
        } else {
            if (blurAlpha == 1.0f) {
                paint.setBlendMode(SkBlendMode::kSrc);
            }
            canvas->drawRect(rect, paint);
        }
        return;
    }

    if (blurRadius < mMaxCrossFadeRadius) {
        LOG_THREAD_STATE_AND_CRASH_IF(!input);

        // For sampling Skia's API expects the inverse of what logically seems appropriate. In this
        // case you might expect the matrix to simply be the canvas matrix.
        SkMatrix inputMatrix;
        if (!canvas->getTotalMatrix().invert(&inputMatrix)) {
            ALOGE("matrix was unable to be inverted");
        }
        if (zoomScale != 1.0f) {
            inputMatrix.preTranslate(
                    blurRect.width() * (1 - zoomScale) / 2.0f,
                    blurRect.height() * (1 - zoomScale) / 2.0f);
            inputMatrix.preScale(zoomScale, zoomScale);
        }

        SkRuntimeShaderBuilder blurBuilder(mMixEffect);
        blurBuilder.child("blurredInput") = blurShader;
        blurBuilder.child("originalInput") =
                input->makeShader(SkTileMode::kMirror, SkTileMode::kMirror, linearSampling,
                                  inputMatrix);

        blurBuilder.uniform("mixFactor") = blurRadius / mMaxCrossFadeRadius;

        paint.setShader(blurBuilder.makeShader());
    } else {
        paint.setShader(blurShader);
    }

    if (effectRegion.isRect()) {
        // Penguin's plain window blurs (material -1, such as home's while an app closes) can come
        // out partly transparent at the screen edges; copying them with kSrc punched those edges
        // through to black, so blend them over what is already drawn instead.
        if (blurAlpha == 1.0f && material >= 0) {
            paint.setBlendMode(SkBlendMode::kSrc);
        }
        canvas->drawRect(effectRegion.rect(), paint);
    } else {
        paint.setAntiAlias(true);
        canvas->drawRRect(effectRegion, paint);
    }
}

} // namespace skia
} // namespace renderengine
} // namespace android
