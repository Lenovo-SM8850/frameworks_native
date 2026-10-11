// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <ui/GraphicBuffer.h>
#include <string>

namespace android::renderengine {
// Inspect producer metadata without modifying the native handle or HWC metadata.
bool dolbyVisionRequiresClientComposition(const sp<GraphicBuffer>& buffer, ui::Dataspace dataspace);
void onDolbyVisionLayerDestroyed(const std::string& name);
} // namespace android::renderengine
