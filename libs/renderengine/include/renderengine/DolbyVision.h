// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <ui/GraphicBuffer.h>
#include <string>
#include <functional>
#include <cstdint>

namespace android::renderengine {
// The callback must only schedule work; it is invoked from asynchronous workers.
void setDolbyVisionRepaintCallback(std::function<void()> callback);
uint64_t getDolbyVisionRenderGeneration();

// Inspect producer metadata without modifying the native handle or HWC metadata.
bool dolbyVisionRequiresClientComposition(const sp<GraphicBuffer>& buffer, ui::Dataspace dataspace);
void onDolbyVisionLayerDestroyed(const std::string& name);
} // namespace android::renderengine
