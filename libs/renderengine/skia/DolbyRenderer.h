// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <functional>

#include <include/core/SkRefCnt.h>
#include <renderengine/DisplaySettings.h>
#include <renderengine/LayerSettings.h>

class SkShader;

namespace android::renderengine::skia {
// A null result leaves the caller's original AOSP color pipeline intact.
// Invoked synchronously only for a ready Dolby frame; never retained by the worker.
sk_sp<SkShader> createDolbyVisionShader(const std::function<sk_sp<SkShader>()>& makeInput,
                                        const LayerSettings& layer, const DisplaySettings& display,
                                        ui::PixelFormat targetFormat, bool vulkan,
                                        float dimmingRatio);
} // namespace android::renderengine::skia
