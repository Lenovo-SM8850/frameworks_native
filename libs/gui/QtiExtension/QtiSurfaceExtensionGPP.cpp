/* Copyright (c) 2024-2025 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#undef LOG_TAG
#define LOG_TAG "SurfaceExtension-GPP"
// #define LOG_NDEBUG 0
#include "dlfcn.h"
#include <cutils/misc.h>
#include <private/android_filesystem_config.h>
#include <cutils/properties.h>
#include "QtiSurfaceExtensionGPP.h"
#include <com_android_graphics_libgui_flags.h>
#include <unordered_map>

using ::android::IGraphicBufferProducer;
using ::android::sp;
using ::android::status_t;
using ::android::IBinder;

typedef status_t (*InitFunc_t)(sp<IGraphicBufferProducer>*, const sp<IBinder>&);
typedef void (*DeinitFunc_t)(const sp<IBinder>&);
static constexpr uint32_t BQ_LAYER_COUNT = 1;
#define GPP_INIT_RETRY_INTERVAL_MS 500
#define GPP_INIT_RETRY_MAX_ATTEMPTS 5

namespace android::libguiextension {
namespace {
// Registering for death makes isBinderAlive reflect a stopped producer without a per-frame IPC.
class GppProducerDeathRecipient final : public IBinder::DeathRecipient {
public:
    void binderDied(const wp<IBinder>&) override {}
};

const sp<IBinder::DeathRecipient> kProducerDeathRecipient =
        sp<GppProducerDeathRecipient>::make();
} // namespace

QtiSurfaceExtensionGPP::QtiSurfaceExtensionGPP(
        Surface* surface,
        const sp<IBinder> handle,
        sp<IGraphicBufferProducer>* gbp)
    : mIsEnable(false),
      mSurface(surface),
      mIsSupported(true),
      mConnectedToGpu(false),
      mClientSetBufferCount(0),
      mSessionConflictRetryCount(0),
      mLastSessionConflictRetryTimestamp(0),
      mOriginalGbp(*gbp),
      mGbp(nullptr),
      mHandle(handle),
      mLibHandler(nullptr),
      mFuncInit(nullptr),
      mFuncDeinit(nullptr),
      mConnectedProducerListener(),
      mLastQueuedBufferSlot(-1),
      mAutoPrerotation(false),
      mPresentMode(0) {
    // FIRST_APPLICATION_UID / AID_APP_START is first uid for 3rd party application.
    // The system application will not enter this logic.
    mUID = getuid();
    if (mUID % AID_USER_OFFSET < FIRST_APPLICATION_UID) {
        mIsSupported = false;
        return;
    }

    mLibHandler = dlopen("libgppextension.so", RTLD_NOW|RTLD_GLOBAL);
    if (!mLibHandler) {
        ALOGV("%s: mHandle %p, failed dlopen libgppextension, err %s",
            __FUNCTION__, mHandle.get(), dlerror());
        mIsSupported = false;
        return;
    }
    mFuncInit = dlsym(mLibHandler, "Init");
    mFuncDeinit = dlsym(mLibHandler, "Deinit");
    if (!mFuncInit || !mFuncDeinit) {
        ALOGV("%s: mHandle %p, failed dlsym functions, err %s",
            __FUNCTION__, mHandle.get(), dlerror());
        dlclose(mLibHandler);
        mLibHandler = nullptr;
        mIsSupported = false;
        return;
    }
    ALOGV("Created Surface Extension for GPP, original buffer producer = %p", mOriginalGbp.get());
}

bool QtiSurfaceExtensionGPP::Connect(int api, sp<IGraphicBufferProducer>* gbp) {
    if (api != NATIVE_WINDOW_API_EGL) {
        ALOGV("Connect api %d is not EGL. No need to enable GPP feather.", api);
        return false;
    }

    std::lock_guard _lock{mMutex};
    mConnectedToGpu = true;
    return DynamicEnableInternal(gbp, false);
}

void QtiSurfaceExtensionGPP::Disconnect(int api, sp<IGraphicBufferProducer>* gbp) {
    if (api == NATIVE_WINDOW_API_EGL || IsGPPEnabled()) {
        std::lock_guard _lock{mMutex};
        mConnectedToGpu = false;
        DisableGPPinternal(gbp);
    }
}

bool QtiSurfaceExtensionGPP::DynamicEnable(sp<IGraphicBufferProducer>* gbp) {
    std::lock_guard _lock{mMutex};
    return DynamicEnableInternal(gbp, true);
}

void QtiSurfaceExtensionGPP::DisableGPPinternal(sp<IGraphicBufferProducer>* gbp) {
    if (mGbp != nullptr) {
        IInterface::asBinder(mGbp)->unlinkToDeath(kProducerDeathRecipient);
    }
    if (mIsEnable && mFuncDeinit) {
        reinterpret_cast<DeinitFunc_t>(mFuncDeinit)(mHandle);
    }
    mGbp = nullptr;
    if (mOriginalGbp != nullptr) {
        if (static_cast<uint32_t>(mFrameRate) != 0) {
            ALOGI("Change back to App set FrameRate %.1f", mFrameRate);
            mOriginalGbp->setFrameRate(mFrameRate, mCompatibility, mChangeFrameRateStrategy);
        }
        *gbp = mOriginalGbp;
        SetGraphicBufferProducer(*gbp);
        mIsEnable = false;
    } else {
        ALOGV("mOriginalGbp is not set.");
    }
}

bool QtiSurfaceExtensionGPP::IsSessionConflictRetryAllowed() const {
    const nsecs_t now = systemTime(SYSTEM_TIME_MONOTONIC);
    const nsecs_t intervalNs = milliseconds_to_nanoseconds(GPP_INIT_RETRY_INTERVAL_MS);
    return mSessionConflictRetryCount < GPP_INIT_RETRY_MAX_ATTEMPTS &&
           (now - mLastSessionConflictRetryTimestamp >= intervalNs);
}

bool QtiSurfaceExtensionGPP::DynamicEnableInternal(sp<IGraphicBufferProducer>* gbp, bool needReconnect) {
    const int property = property_get_int32("vendor.gpp.frc.enable", 0x11);
    // A failed session attach is not a permanent surface capability decision. A later
    // off/on choice must retry on the same producer after the service becomes ready.
    if (property != mLastEnableProperty) {
        mLastEnableProperty = property;
        mSessionConflictRetryCount = 0;
        mLastSessionConflictRetryTimestamp = 0;
        if (mFuncInit && mFuncDeinit && mUID % AID_USER_OFFSET >= FIRST_APPLICATION_UID) {
            mIsSupported = true;
        }
    }
    if (mIsSupported && mConnectedToGpu) {
        if (!mSurface->mDequeuedSlots.empty() || mSurface->mSharedBufferMode) {
            return false;
        }
        // Accept only the documented dynamic states. Malformed properties stay inactive.
        int enable = -1;
        if (property == 0x21) {  // 0x21 dynamic off
            enable = 0;
        } else if (property == 0x22) {  // 0x22 dynamic on
            enable = 1;
        }

        // A paused surface may miss the off interval while the service restarts.
        // Validate the retained producer before reusing it for an enabled session.
        if (enable == 1 && mIsEnable && mGbp != nullptr &&
            !IInterface::asBinder(mGbp)->isBinderAlive()) {
            DisableGPPinternal(gbp);
            mSessionConflictRetryCount = 0;
            mLastSessionConflictRetryTimestamp = 0;
        }

        ALOGV("Property enable = %d, before dynamic enable, mGraphicBufferProducer = %p ", enable, gbp->get());
        if (enable !=-1 && mIsEnable != enable) {
            if (enable) {
                if (!IsSessionConflictRetryAllowed()) {
                    return false;
                }
                ALOGV("Enabling GPP feather");
                status_t err = reinterpret_cast<InitFunc_t>(mFuncInit)(gbp, mHandle);
                if (err == OK) {
                    mIsEnable = true;
                    mIsSupported = true;
                    mSessionConflictRetryCount = 0;
                    mLastSessionConflictRetryTimestamp = 0;
                    mGbp = *gbp;
                    IInterface::asBinder(mGbp)->linkToDeath(kProducerDeathRecipient);
                    if (static_cast<uint32_t>(mFrameRate) != 0 && mOriginalGbp) {
                        ALOGI("App set FrameRate %.1f, overwrite with 0.0 after GPP enabled",
                            mFrameRate);
                        mOriginalGbp->setFrameRate(0.0f, mCompatibility, mChangeFrameRateStrategy);
                    }
                } else {
                    *gbp = mOriginalGbp;
                    mIsEnable = false;
                    if (err == NAME_NOT_FOUND || err == INVALID_OPERATION) {
                       mIsSupported = false;
                       ALOGV("Failed to init GPP: Surface or App is not supported by GPP");
                    } else if (err == ALREADY_EXISTS) {
                        mLastSessionConflictRetryTimestamp = systemTime(SYSTEM_TIME_MONOTONIC);
                        mSessionConflictRetryCount++;
                        if (mSessionConflictRetryCount >= GPP_INIT_RETRY_MAX_ATTEMPTS) {
                            mIsSupported = false;
                        }
                        ALOGE("Failed to init GPP: Exist another session. Retry attempt %d", mSessionConflictRetryCount);
                    } else {
                        mIsSupported = false;
                        ALOGE("Failed to init GPP: Unknown error.");
                    }
                }
            } else {
                ALOGV("Disabling GPP feather");
                DisableGPPinternal(gbp);
            }
            if (needReconnect && mIsEnable == enable && nullptr != *gbp && nullptr != mConnectedProducerListener) {
                IGraphicBufferProducer::QueueBufferOutput output;
                (*gbp)->connect(mConnectedProducerListener, mAPI, mProducerControlledByApp, &output);
                (*gbp)->setAutoPrerotation(mAutoPrerotation);
                if (mPresentMode != 0) {
                    (*gbp)->setPresentMode(mPresentMode);
                }
                TransferBuffersToNewQueue(gbp);
            }
            if (mIsEnable == enable) {
                ALOGV("GPP dynamic On/Off succeeded, mIsEnable = %d", mIsEnable);
                return true;
            } else {
                ALOGV("GPP dynamic On/Off failed, mIsEnable = %d", mIsEnable);
                return false;
            }

        } else {
            ALOGV("No need to change BufferProducer");
            return true;
        }

        ALOGV("After dynamic enable, mGraphicBufferProducer = %p, mOriginalGbp = %p", gbp->get(), mOriginalGbp.get());
    } else {
        ALOGV("Unsupport Surface");
        return true;
    }
}

 void QtiSurfaceExtensionGPP::StoreConnect(int api, const sp<IProducerListener>& listener, bool producerControlledByApp) {
   std::lock_guard lock(mMutex);
   mAPI = api;
   mConnectedProducerListener = listener;
   mProducerControlledByApp = producerControlledByApp;
}

 void QtiSurfaceExtensionGPP::SetGraphicBufferProducer(sp<IGraphicBufferProducer> gbp) {
    if (gbp != nullptr) {
        if (mSidebandStream.seted)
            gbp->setSidebandStream(mSidebandStream.stream);
    }
}

int QtiSurfaceExtensionGPP::query(int what, int *outValue) const {
    std::lock_guard _lock{mMutex};

    if (mOriginalGbp == nullptr) {
        ALOGE("mIsEnable = %d , mOriginalGbp must not be NULL", mIsEnable);
        return BAD_VALUE;
    }

    if (mIsEnable == true) {
        switch (what) {
            case NATIVE_WINDOW_WIDTH:
            case NATIVE_WINDOW_HEIGHT:
            case NATIVE_WINDOW_FORMAT:
            case NATIVE_WINDOW_CONSUMER_USAGE_BITS:
            case NATIVE_WINDOW_STICKY_TRANSFORM:
            case NATIVE_WINDOW_CONSUMER_RUNNING_BEHIND:
            case NATIVE_WINDOW_DEFAULT_DATASPACE:
            case NATIVE_WINDOW_BUFFER_AGE:
            case NATIVE_WINDOW_CONSUMER_IS_PROTECTED:
                // Call the mOriginalGbp directly to avoid unnecessary binder call due to those values of mGbp are the same as mOriginalGbp.
                return mOriginalGbp->query(what, outValue);
            case NATIVE_WINDOW_LAYER_COUNT:
                // All BufferQueue buffers have a single layer.
                *outValue = BQ_LAYER_COUNT;
                return NO_ERROR;
            case NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS:
                if (mGbp != nullptr) {
                    return mGbp->query(what, outValue);
                } else {
                    ALOGW("mGbp is NULL");
                    return BAD_VALUE;
                }
            default:
                return BAD_VALUE;
        }
    } else {
        return mOriginalGbp->query(what, outValue);
    }
}

QtiSurfaceExtensionGPP::~QtiSurfaceExtensionGPP() {
    if (mIsEnable && mFuncDeinit) {
        reinterpret_cast<DeinitFunc_t>(mFuncDeinit)(mHandle);
    }
    if (mLibHandler != nullptr) {
        dlclose(mLibHandler);
    }
    mLibHandler = nullptr;
    ALOGV("~QtiSurfaceExtensionGPP()");
}

void QtiSurfaceExtensionGPP::TransferBuffersToNewQueue(sp<IGraphicBufferProducer>* gbp) {
    if (mSurface == nullptr) {
        ALOGE("NULL Surface");
        return;
    }
    int count = 0;
    for (int slot = 0; slot < (int)mSurface->mSlots.size(); ++slot) {
        if (mSurface->mSlots[slot].buffer != nullptr) {
            count++;
        }
    }
    int minUndequeuedBuffers = 0;
    int maxDequeuedBufferCount = 0;
    if (mClientSetBufferCount == 0) {
        maxDequeuedBufferCount = 2;
    } else {
        if ((*gbp)->query(
            NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS, &minUndequeuedBuffers) == NO_ERROR) {
            maxDequeuedBufferCount = mClientSetBufferCount - minUndequeuedBuffers;
        } else {
            maxDequeuedBufferCount = 2;
        }
    }
    ALOGI("Min undequeued count %d, max dequeued count %d",
        minUndequeuedBuffers, maxDequeuedBufferCount);
    if ((count == 0) || (count != mClientSetBufferCount)) {
        ALOGI("Free buffer count %d mismatch client set buffer count %d",
            count, mClientSetBufferCount);
        (*gbp)->allowAllocation(true);
        (*gbp)->setMaxDequeuedBufferCount(maxDequeuedBufferCount);
        return;
    }
    // App should only uses existing buffers rather than dequeue any new
    // slot and request new buffers for dynamic ON/OFF case
    (*gbp)->allowAllocation(false);
    (*gbp)->setMaxDequeuedBufferCount(maxDequeuedBufferCount);
    ALOGI("DisAllow dequeue new slots and set max dequeued buffer count %d",
        maxDequeuedBufferCount);

    std::unordered_map<int, sp<GraphicBuffer>> newSlotsToBuffersMapping {};
    // All callers hold Surface::mMutex while changing producers and slot mappings.
	int lastQueuedBufferSlot = -1;
    for (int slot = 0; slot < (int)mSurface->mSlots.size(); ++slot) {
        if (mSurface->mSlots[slot].buffer != nullptr) {
            if (slot == mLastQueuedBufferSlot) {
                lastQueuedBufferSlot = mLastQueuedBufferSlot;
                ALOGI("mLastQueuedBufferSlot: %d", mLastQueuedBufferSlot);
                continue;
            }
            int newSlot = -1;
            status_t result = (*gbp)->attachBuffer(&newSlot, mSurface->mSlots[slot].buffer);
            if (result == NO_ERROR && newSlot != -1) {
                newSlotsToBuffersMapping.emplace(newSlot, std::move(mSurface->mSlots[slot].buffer));
                ALOGI("Same buffer %p is mapped from old slot=%d to new slot=%d",
                    &(mSurface->mSlots[slot].buffer), slot, newSlot);
                (*gbp)->cancelBuffer(newSlot, Fence::NO_FENCE);
            }
        }
    }

    if (lastQueuedBufferSlot >= 0) {
        int slot = lastQueuedBufferSlot;
        int newSlot = -1;
        status_t result = (*gbp)->attachBuffer(&newSlot, mSurface->mSlots[slot].buffer);
        if (result == NO_ERROR && newSlot != -1) {
            newSlotsToBuffersMapping.emplace(newSlot, std::move(mSurface->mSlots[slot].buffer));
            ALOGI("Same buffer %p is mapped from old slot=%d to new slot=%d",
                &(mSurface->mSlots[slot].buffer), slot, newSlot);
            (*gbp)->cancelBuffer(newSlot, Fence::NO_FENCE);
        }
    }

    for (auto& [newSlot, buffer] : newSlotsToBuffersMapping) {
        if (static_cast<size_t>(newSlot) >= mSurface->mSlots.size()) {
            mSurface->mSlots.resize(static_cast<size_t>(newSlot) + 1);
        }
        mSurface->mSlots[newSlot].buffer = std::move(buffer);
    }
    newSlotsToBuffersMapping.clear();
}

void QtiSurfaceExtensionGPP::setFrameRate(float frameRate, int8_t compatibility,
    int8_t changeFrameRateStrategy) {
    std::lock_guard lock(mMutex);
    ALOGI("FrameRate %.1f, compatibility %d changeFrameRateStrategy %d",
        frameRate, compatibility, changeFrameRateStrategy);
    mFrameRate = frameRate;
    mCompatibility = compatibility;
    mChangeFrameRateStrategy = changeFrameRateStrategy;
}

void QtiSurfaceExtensionGPP::setPresentMode(int32_t mode) {
    std::lock_guard lock(mMutex);
    ALOGI("%s: present mode %" PRId32, __func__, mode);
    mPresentMode = mode;
}

} //namespace android::libguiextension