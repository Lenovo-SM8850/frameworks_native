// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <functional>

#include <include/core/SkRefCnt.h>
#include <renderengine/DisplaySettings.h>
#include <renderengine/LayerSettings.h>

class SkShader;

namespace android::renderengine::skia {
enum class DolbyShaderStatus { NotDolby, Pending, Ready, Error };
struct DolbyShaderResult {
    DolbyShaderStatus status = DolbyShaderStatus::NotDolby;
    sk_sp<SkShader> shader;
};
// Pending must be held; NotDolby and Error use the original AOSP pipeline.
// Invoked synchronously only for a ready Dolby frame; never retained by the worker.
DolbyShaderResult createDolbyVisionShader(const std::function<sk_sp<SkShader>()>& makeInput,
                                        const LayerSettings& layer, const DisplaySettings& display,
                                        ui::PixelFormat targetFormat, bool vulkan,
                                        float dimmingRatio);
} // namespace android::renderengine::skia
