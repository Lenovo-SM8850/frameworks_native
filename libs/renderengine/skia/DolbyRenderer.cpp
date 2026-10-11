// SPDX-License-Identifier: Apache-2.0

#include "DolbyRenderer.h"

#include <aidl/vendor/dolby/dvs/BnDvsCallback.h>
#include <aidl/vendor/dolby/dvs/IDvs.h>
#include <android/binder_ibinder.h>
#include <android/binder_manager.h>
#include <dlfcn.h>
#include <log/log.h>
#include <renderengine/ColorSpaces.h>
#include <renderengine/DolbyVision.h>
#include <renderengine/ExternalTexture.h>

#include <include/core/SkBitmap.h>
#include <include/core/SkColorSpace.h>
#include <include/core/SkImage.h>
#include <include/core/SkShader.h>
#include <include/effects/SkRuntimeEffect.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace android::renderengine {
namespace {

using Metadata = std::optional<std::vector<uint8_t>>;
using Uniform = std::pair<std::string, std::vector<uint8_t>>;
using Uniforms = std::vector<Uniform>;
using aidl::vendor::dolby::dvs::IDvs;
constexpr size_t kMaxPayload = 42 * 1024;
constexpr std::array<uint8_t, 7> kVideoTag = {0, 0, 0, 1, 0x7c, 1, 0x19};
constexpr std::array<uint8_t, 7> kAlternateTag = {5, 0x20, 0xb6, 1, 0x32, 0x7d, 2};

// Mobile_3.5.1.93b5ab28 arm64 ABI, recovered from the stock system blobs.
// Color metadata is copied as two 16-byte loads; uniform entries have stride 48.
// The mapper facade contains exactly one pointer to its allocated implementation.
struct ColorMetadata {
    uint32_t primaries = 1;
    uint32_t range = 255;
    uint32_t transfer = 1;
    uint32_t reserved = 0;
    float maxLuminance = 0;
    std::array<uint32_t, 3> padding = {};
};
static_assert(sizeof(ColorMetadata) == 32);
static_assert(sizeof(Uniform) == 48);
static_assert(sizeof(Metadata) == 32);
static_assert(sizeof(void*) == 8);

ColorMetadata colorMetadata(ui::Dataspace dataspace, float luminance, bool source) {
    ColorMetadata value;
    const uint32_t bits = static_cast<uint32_t>(dataspace);
    const uint32_t standard = (bits & HAL_DATASPACE_STANDARD_MASK) >> HAL_DATASPACE_STANDARD_SHIFT;
    constexpr uint32_t primaries[] = {9, 9, 1, 1, 12};
    if (standard >= 6 && standard <= 10) value.primaries = primaries[standard - 6];
    const uint32_t transfer = (bits & HAL_DATASPACE_TRANSFER_MASK) >> HAL_DATASPACE_TRANSFER_SHIFT;
    constexpr uint32_t sourceTransfers[] = {6, 4, 3, 5, 16, 18};
    constexpr uint32_t targetTransfers[] = {8, 1, 6, 4, 3, 5, 16, 18};
    if (source && transfer >= 3 && transfer <= 8) {
        value.transfer = sourceTransfers[transfer - 3];
    } else if (!source && transfer >= 1 && transfer <= 8) {
        value.transfer = targetTransfers[transfer - 1];
    }
    switch (bits & HAL_DATASPACE_RANGE_MASK) {
        case HAL_DATASPACE_RANGE_FULL:
            value.range = 1;
            break;
        case HAL_DATASPACE_RANGE_LIMITED:
            value.range = 0;
            break;
        case HAL_DATASPACE_RANGE_EXTENDED:
            value.range = 2;
            break;
    }
    value.maxLuminance = luminance;
    return value;
}

// The proprietary interface does not publish CFI type information for our
// reconstructed C++ declarations. Confine the ABI boundary to these calls;
// do not disable CFI for RenderEngine or SurfaceFlinger as a whole.
template <size_t Slot, typename Return, typename... Args>
__attribute__((noinline, no_sanitize("cfi"))) Return effectCall(void* object, Args... args) {
    auto table = *static_cast<void***>(object);
    return reinterpret_cast<Return (*)(void*, Args...)>(table[Slot])(object, args...);
}

template <typename Return, typename... Args>
__attribute__((noinline, no_sanitize("cfi"))) Return blobCall(void* function, Args... args) {
    return reinterpret_cast<Return (*)(Args...)>(function)(args...);
}

class DolbyRenderer;
DolbyRenderer& renderer();
std::atomic<DolbyRenderer*> existingRenderer = nullptr;

class Callback final : public aidl::vendor::dolby::dvs::BnDvsCallback {
public:
    ndk::ScopedAStatus onDolbyVisionPlaybackStart(bool* result) override;
    ndk::ScopedAStatus onDolbyVisionPlaybackStop(bool* result) override;
    ndk::ScopedAStatus onDolbyNotification(const std::string& notification) override;
};

class DolbyRenderer {
public:
    DolbyRenderer() {
        // Process-lifetime worker: no blob loading, service discovery or Binder
        // transaction runs on the render thread, even on first use.
        std::thread([this] { controlLoop(); }).detach();
    }

    std::atomic<bool> serviceDied = false;
    std::atomic<bool> serviceReady = false;
    std::atomic<uint64_t> epoch = 0;

    // The mapper facade is immutable after release-publication by load(). Its
    // getters only query the already loaded, in-process gralloc mapper.
    bool load() {
        if (mLoadAttempted) return mAvailable.load();
        mLoadAttempted = true;
        // Absolute system paths avoid accidentally loading the vendor ABI variants.
        mUi = dlopen("/system/lib64/libdolbyui.so", RTLD_NOW | RTLD_LOCAL);
        mProxy = dlopen("/system/lib64/libdolbyproxyandroid.so", RTLD_NOW | RTLD_LOCAL);
        mEffectLibrary = dlopen("/system/lib64/libdolbyeffect.so", RTLD_NOW | RTLD_LOCAL);
        if (!mUi || !mProxy || !mEffectLibrary) {
            ALOGW("DolbyRenderer: system Dolby blobs unavailable; using AOSP rendering: %s",
                  dlerror());
            return false;
        }
        mMapperCtor = dlsym(mUi, "_ZN11dolbyvision21DVGraphicBufferMapperC1Ev");
        mGetMetadata = dlsym(mUi,
                             "_ZN11dolbyvision21DVGraphicBufferMapper22getDolbyVisionMetadataEPK13n"
                             "ative_handlePNSt3__18optionalINS4_6vectorIhNS4_9allocatorIhEEEEEERb");
        mGetTimestamp = dlsym(mUi,
                              "_ZN11dolbyvision21DVGraphicBufferMapper17getVideoTimestampEPK13nativ"
                              "e_handleRm");
        mInstantiate = dlsym(mEffectLibrary, "_ZN11dolbyVision11DolbyEffect11instantiateEv");
        if (!mMapperCtor || !mGetMetadata || !mGetTimestamp || !mInstantiate) {
            ALOGW("DolbyRenderer: incompatible system Dolby API; using AOSP rendering");
            return false;
        }
        blobCall<void>(mMapperCtor, &mMapper);
        mAvailable.store(true, std::memory_order_release);
        ALOGI("DolbyRenderer: system Dolby Vision renderer available");
        return true;
    }

    std::optional<std::vector<uint8_t>> read(const sp<GraphicBuffer>& buffer) {
        if (!buffer || !mAvailable.load(std::memory_order_acquire)) return std::nullopt;
        Metadata metadata;
        bool custom = false;
        if (blobCall<int>(mGetMetadata, &mMapper, buffer->handle, &metadata, &custom) != 0 ||
            !metadata || metadata->empty())
            return std::nullopt;
        if (custom) {
            if (metadata->size() < sizeof(uint64_t)) return std::nullopt;
            uint64_t size;
            memcpy(&size, metadata->data(), sizeof(size));
            if (size > kMaxPayload || size > metadata->size() - sizeof(size)) return std::nullopt;
            metadata = std::vector<uint8_t>(metadata->begin() + sizeof(size),
                                            metadata->begin() + sizeof(size) + size);
        }
        if (metadata->size() > kMaxPayload) return std::nullopt;
        return metadata;
    }

    uint64_t timestamp(const sp<GraphicBuffer>& buffer) {
        uint64_t value = 0;
        if (buffer && mAvailable.load(std::memory_order_acquire))
            blobCall<int>(mGetTimestamp, &mMapper, buffer->handle, &value);
        return value;
    }

    void connectService() {
        if (serviceDied.exchange(false)) {
            serviceReady.store(false);
            mService.reset();
            destroyEffect();
        }
        if (mService) return;
        const auto now = std::chrono::steady_clock::now();
        if (now - mLastServiceCheck < std::chrono::seconds(1)) return;
        mLastServiceCheck = now;
        ndk::SpAIBinder binder(AServiceManager_checkService("vendor.dolby.dvs.IDvs/default"));
        if (!binder.get()) return;
        auto service = IDvs::fromBinder(binder);
        if (!service) return;
        if (!mCallback) mCallback = ndk::SharedRefBase::make<Callback>();
        if (!service->registerCallback(mCallback).isOk()) return;
        if (!mDeathRecipient) {
            mDeathRecipient = AIBinder_DeathRecipient_new([](void* cookie) {
                auto& state = *static_cast<DolbyRenderer*>(cookie);
                state.serviceReady.store(false);
                state.epoch.fetch_add(1);
                state.serviceDied.store(true);
            });
        }
        if (AIBinder_linkToDeath(binder.get(), mDeathRecipient, this) != STATUS_OK) return;
        mService = std::move(service);
        if (!serviceDied.load()) serviceReady.store(true);
    }

    void destroyEffect() {
        if (mEffect) effectCall<14, void>(mEffect);
        mEffect = nullptr;
        mNames.clear();
    }

    void removeEffectLayer(const std::string& name) {
        const auto entry = mNames.find(name);
        if (entry == mNames.end()) return;
        const auto id = entry->second;
        mNames.erase(entry);
        for (const auto& [otherName, otherId] : mNames) {
            if (otherId == id) return;
        }
        if (mEffect) effectCall<12, void>(mEffect, id);
        if (mNames.empty()) destroyEffect();
    }

    struct Request {
        sp<GraphicBuffer> buffer;
        std::vector<uint8_t> metadata;
        std::string name;
        ColorMetadata source;
        ColorMetadata target;
        ui::Dataspace outputDataspace = ui::Dataspace::UNKNOWN;
        float dimming = 1;
        float luminance = 0;
        uint32_t id = 0;
        bool fp16 = false;
        bool vulkan = false;
        uint64_t serial = 0;
        uint64_t timestamp = 0;
    };
    struct Frame {
        std::vector<uint8_t> metadata;
        uint64_t timestamp;
        ColorMetadata source;
        ColorMetadata target;
        ui::Dataspace outputDataspace;
        float dimming;
        float luminance;
        uint32_t id;
        bool fp16;
        bool vulkan;
        uint64_t epoch;
        uint32_t size;
        std::vector<uint8_t> pixels;
        Uniforms uniforms;
        std::string text;
        std::chrono::steady_clock::time_point generated;

        bool matches(const Request& request, bool allowToneMappingLag = false) const {
            return id == request.id && fp16 == request.fp16 && vulkan == request.vulkan &&
                    outputDataspace == request.outputDataspace &&
                    (allowToneMappingLag ||
                     (dimming == request.dimming && luminance == request.luminance)) &&
                    memcmp(&source, &request.source, sizeof(source)) == 0 &&
                    memcmp(&target, &request.target, sizeof(target)) == 0;
        }
    };

    std::shared_ptr<const Frame> generate(Request& request) {
        if (!mEffect) {
            mEffect = blobCall<void*>(mInstantiate);
            if (!mEffect) return nullptr;
            effectCall<8, void>(mEffect); // Stock enableInverseMatrixNoDVSKPatch.
        }
        effectCall<9, void>(mEffect, uint32_t(request.vulkan ? 1 : 0));
        const auto id = request.id;
        effectCall<3, void>(mEffect, &request.source, id);
        effectCall<2, void>(mEffect, request.metadata.data(), uint32_t(request.metadata.size()),
                            request.timestamp, id);
        effectCall<0, void>(mEffect, request.fp16);
        auto frame = std::make_shared<Frame>();
        frame->metadata = request.metadata;
        frame->timestamp = request.timestamp;
        // Stock clears this field only in its copied vector, after submission.
        memset(request.metadata.data() + 3, 0, 4);
        if (!mNames.contains(request.name)) {
            ALOGI("DolbyRenderer: Adding dolby layer name:%s, layer id:%u", request.name.c_str(),
                  id);
        }
        mNames[request.name] = id;
        effectCall<4, void>(mEffect, &request.target);
        effectCall<11, void>(mEffect, request.dimming);
        effectCall<1, void>(mEffect, request.luminance);
        uint32_t size = 0;
        auto pixels = effectCall<5, const uint8_t*>(mEffect, &size, id);
        if (!pixels || size < 2 || size > 64) return nullptr;
        auto uniforms = effectCall<7, Uniforms>(mEffect, id);
        const char* text = effectCall<6, const char*>(mEffect);
        if (!text || strnlen(text, 64 * 1024) == 64 * 1024) return nullptr;
        frame->source = request.source;
        frame->target = request.target;
        frame->outputDataspace = request.outputDataspace;
        frame->dimming = request.dimming;
        frame->luminance = request.luminance;
        frame->id = request.id;
        frame->fp16 = request.fp16;
        frame->vulkan = request.vulkan;
        frame->epoch = epoch.load();
        frame->size = size;
        frame->pixels.assign(pixels, pixels + size_t(size) * size * size * 4);
        frame->uniforms = std::move(uniforms);
        frame->text = text;
        frame->generated = std::chrono::steady_clock::now();
        return frame;
    }

    sk_sp<SkShader> shader(const std::function<sk_sp<SkShader>()>& makeInput, Request request,
                           ui::Dataspace outputDataspace) {
        std::shared_ptr<const Frame> frame;
        Request retiredRequest;
        request.outputDataspace = outputDataspace;
        bool compatibleFrame = false;
        bool matchingMetadata = false;
        // Only move queue entries / shared snapshots under this mutex. The
        // worker never holds it during blob calls, Binder or LUT generation.
        {
            std::lock_guard lock(mQueueMutex);
            request.serial = ++mSerials[request.name];
            if (const auto entry = mFrames.find(request.name); entry != mFrames.end())
                frame = entry->second;
            compatibleFrame = frame && frame->matches(request, true);
            matchingMetadata = compatibleFrame && frame->metadata == request.metadata &&
                    frame->timestamp == request.timestamp;
            auto& queued = mRequests[request.name];
            retiredRequest = std::move(queued);
            queued = std::move(request);
        }
        mWake.notify_one();
        // A paused buffer can be recomposed long after its LUT was generated.
        // Identical metadata, timestamp and compatible color state remain valid;
        // only the previous-frame playback allowance is bounded to 100 ms.
        if (!serviceReady.load() || !frame || frame->epoch != epoch.load() || !compatibleFrame ||
            (!matchingMetadata && std::chrono::steady_clock::now() - frame->generated >
                            std::chrono::milliseconds(100)))
            return nullptr;
        // Keep the previous LUT and its uniforms together while dimming or
        // display luminance catches up. Their tone mapping can lag, but the
        // source, target encoding, format and backend must still match.
        std::lock_guard lock(mShaderMutex);
        auto& runtimeEffect = mRuntimeEffects[frame->fp16];
        auto& sourceText = mRuntimeTexts[frame->fp16];
        if (!runtimeEffect || sourceText != frame->text) {
            auto result = SkRuntimeEffect::MakeForShader(SkString(frame->text.c_str()));
            if (!result.effect) {
                ALOGW("DolbyRenderer: SkSL compilation failed: %s", result.errorText.c_str());
                return nullptr;
            }
            runtimeEffect = std::move(result.effect);
            sourceText = frame->text;
        }
        const auto size = frame->size;
        if (!runtimeEffect->findChild("child") || !runtimeEffect->findChild("lut_cube"))
            return nullptr;
        SkBitmap lut;
        // Stock packs each LUT entry as R | G << 10 | B << 20 (0x9952c/34).
        // Its SkColorType 9 is RGB_101010x, not RGBA8 or float RGBA.
        const auto info =
                SkImageInfo::Make(size * size, size, kRGB_101010x_SkColorType, kOpaque_SkAlphaType,
                                  toSkColorSpace(outputDataspace, ColorSpaceOptions::None));
        if (!lut.tryAllocPixels(info)) return nullptr;
        memcpy(lut.getPixels(), frame->pixels.data(), frame->pixels.size());
        lut.setImmutable();
        auto image = SkImages::RasterFromBitmap(lut);
        if (!image) return nullptr;
        const auto matrix = SkMatrix::Scale(1.f / (size * size), 1.f / size);
        auto lutShader = image->makeShader(SkSamplingOptions(SkFilterMode::kLinear), &matrix);
        if (!lutShader) return nullptr;
        SkRuntimeShaderBuilder builder(runtimeEffect);
        for (const auto& [name, bytes] : frame->uniforms) {
            const auto* uniform = runtimeEffect->findUniform(name);
            if (!uniform) continue;
            if (uniform->sizeInBytes() != bytes.size()) return nullptr;
            if (!builder.uniform(name).set(bytes.data(), bytes.size())) return nullptr;
        }
        auto input = makeInput();
        if (!input) return nullptr;
        builder.child("child") = std::move(input);
        builder.child("lut_cube") = std::move(lutShader);
        return builder.makeShader();
    }

    void removeLayer(const std::string& name) {
        std::shared_ptr<const Frame> retired;
        Request retiredRequest;
        {
            std::lock_guard lock(mQueueMutex);
            const auto entry = mSerials.find(name);
            if (entry == mSerials.end()) return;
            const auto serial = ++entry->second;
            Request removal;
            removal.name = name;
            removal.serial = serial;
            auto& queued = mRequests[name];
            retiredRequest = std::move(queued);
            queued = std::move(removal);
            if (const auto entry = mFrames.find(name); entry != mFrames.end()) {
                retired = std::move(entry->second);
                mFrames.erase(entry);
            }
        }
        mWake.notify_one();
    }

private:
    void controlLoop() {
        if (!load()) return;
        std::map<std::string, Request> requests;
        for (;;) {
            std::map<std::string, Request> incoming;
            {
                std::unique_lock lock(mQueueMutex);
                mWake.wait_for(lock, std::chrono::milliseconds(250),
                               [this] { return !mRequests.empty(); });
                incoming.swap(mRequests);
            }
            for (auto& [name, request] : incoming)
                requests.insert_or_assign(name, std::move(request));
            // No render-needed mutex is held anywhere below, including effect
            // teardown, which can also call the proprietary Binder proxy.
            bool hasDolbyBuffer = false;
            for (const auto& [name, request] : requests) hasDolbyBuffer |= bool(request.buffer);
            if (serviceDied.load() || hasDolbyBuffer) connectService();
            for (auto entry = requests.begin(); entry != requests.end();) {
                auto& [name, request] = *entry;
                if (!request.buffer) {
                    removeEffectLayer(name);
                    {
                        std::lock_guard lock(mQueueMutex);
                        const auto serial = mSerials.find(name);
                        if (serial != mSerials.end() && serial->second == request.serial)
                            mSerials.erase(serial);
                    }
                    entry = requests.erase(entry);
                    continue;
                }
                // Keep the latest request while registration is pending/failed,
                // so throttled retries do not depend on another rendered frame.
                if (!serviceReady.load()) {
                    ++entry;
                    continue;
                }
                bool current;
                {
                    std::lock_guard lock(mQueueMutex);
                    const auto serial = mSerials.find(name);
                    current = serial != mSerials.end() && serial->second == request.serial;
                }
                if (current) {
                    const auto generation = epoch.load();
                    auto frame = generate(request);
                    std::shared_ptr<const Frame> retired;
                    {
                        std::lock_guard lock(mQueueMutex);
                        const auto serial = mSerials.find(name);
                        if (frame && serial != mSerials.end() &&
                            serial->second == request.serial && serviceReady.load() &&
                            epoch.load() == generation) {
                            retired = std::move(mFrames[name]);
                            mFrames[name] = std::move(frame);
                        }
                    }
                }
                entry = requests.erase(entry);
            }
        }
    }

    std::mutex mQueueMutex;
    std::condition_variable mWake;
    std::map<std::string, Request> mRequests;
    std::map<std::string, uint64_t> mSerials;
    std::map<std::string, std::shared_ptr<const Frame>> mFrames;
    std::mutex mShaderMutex;
    std::array<std::string, 2> mRuntimeTexts;

    bool mLoadAttempted = false;
    std::atomic<bool> mAvailable = false;
    void* mUi = nullptr;
    void* mProxy = nullptr;
    void* mEffectLibrary = nullptr;
    void* mMapper = nullptr;
    void* mMapperCtor = nullptr;
    void* mGetMetadata = nullptr;
    void* mGetTimestamp = nullptr;
    void* mInstantiate = nullptr;
    void* mEffect = nullptr;
    std::map<std::string, uint32_t> mNames;
    std::array<sk_sp<SkRuntimeEffect>, 2> mRuntimeEffects;
    std::shared_ptr<IDvs> mService;
    std::shared_ptr<Callback> mCallback;
    AIBinder_DeathRecipient* mDeathRecipient = nullptr;
    std::chrono::steady_clock::time_point mLastServiceCheck;
};

DolbyRenderer& renderer() {
    // The mapper, Binder callbacks and dlopen handles share process lifetime.
    // No callback can outlive this object during SurfaceFlinger teardown.
    static auto* instance = new DolbyRenderer;
    existingRenderer.store(instance, std::memory_order_release);
    return *instance;
}

// Playback notifications describe producer activity, but its last Dolby
// buffer can remain visible. Layer removal and service death own teardown.
ndk::ScopedAStatus Callback::onDolbyVisionPlaybackStart(bool* result) {
    *result = true;
    return ndk::ScopedAStatus::ok();
}
ndk::ScopedAStatus Callback::onDolbyVisionPlaybackStop(bool* result) {
    *result = true;
    return ndk::ScopedAStatus::ok();
}
ndk::ScopedAStatus Callback::onDolbyNotification(const std::string&) {
    return ndk::ScopedAStatus::ok();
}
} // namespace

bool dolbyVisionRequiresClientComposition(const sp<GraphicBuffer>& buffer,
                                          ui::Dataspace dataspace) {
    const auto transfer = static_cast<uint32_t>(dataspace) & HAL_DATASPACE_TRANSFER_MASK;
    if (transfer != HAL_DATASPACE_TRANSFER_ST2084 && transfer != HAL_DATASPACE_TRANSFER_HLG)
        return false;
    auto& state = renderer();
    auto metadata = state.read(buffer);
    constexpr char kMobile[] = "mobile";
    if (!metadata || metadata->size() < 16 ||
        memcmp(metadata->data(), kVideoTag.data(), kVideoTag.size()) != 0 ||
        memcmp(metadata->data() + 7, kMobile, 6) != 0)
        return false;
    const size_t offset = (*metadata)[13] | (size_t((*metadata)[14]) << 8);
    return offset + 8 < metadata->size() && (*metadata)[offset + 8] == 0;
}

void onDolbyVisionLayerDestroyed(const std::string& name) {
    if (auto* state = existingRenderer.load(std::memory_order_acquire)) state->removeLayer(name);
}

namespace skia {
sk_sp<SkShader> createDolbyVisionShader(const std::function<sk_sp<SkShader>()>& makeInput,
                                        const LayerSettings& layer, const DisplaySettings& display,
                                        ui::PixelFormat targetFormat, bool vulkan,
                                        float dimmingRatio) {
    if (!makeInput || !layer.source.buffer.buffer) return nullptr;
    // Dolby accepts one RGB dimming scalar; preserve AOSP's full matrix path
    // for channel mixing, translation, alpha changes or amplification.
    const float scalar = layer.colorTransform[0][0];
    if (!std::isfinite(scalar) || scalar < 0 || scalar > 1 ||
        layer.colorTransform != mat4::scale(vec4(scalar, scalar, scalar, 1)))
        return nullptr;
    auto& state = renderer();
    auto buffer = layer.source.buffer.buffer->getBuffer();
    auto metadata = state.read(buffer);
    if (!metadata || metadata->size() < kVideoTag.size()) return nullptr;
    const uint32_t id = memcmp(metadata->data(), kVideoTag.data(), kVideoTag.size()) == 0 ? 1
            : memcmp(metadata->data(), kAlternateTag.data(), kAlternateTag.size()) == 0   ? 2
                                                                                          : 0;
    // Non-Dolby buffers never enqueue control work or touch DVS / effect state.
    if (!id) return nullptr;
    DolbyRenderer::Request request;
    // Capture metadata and its timestamp together before the producer can
    // recycle this buffer while control work is pending. This is local gralloc.
    request.timestamp = state.timestamp(buffer);
    request.buffer = std::move(buffer);
    request.metadata = std::move(*metadata);
    request.name = layer.name;
    request.source =
            colorMetadata(layer.sourceDataspace, layer.source.buffer.maxLuminanceNits, true);
    request.target = colorMetadata(display.outputDataspace, 0, false);
    request.dimming = scalar * dimmingRatio;
    request.luminance = display.currentLuminanceNits;
    request.id = id;
    request.fp16 = targetFormat == ui::PixelFormat::RGBA_FP16;
    request.vulkan = vulkan;
    return state.shader(makeInput, std::move(request), display.outputDataspace);
}
} // namespace skia
} // namespace android::renderengine
