/* Copyright (c) 2024-2025 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
#pragma once

#include <utils/RefBase.h>
#include <utils/Timers.h>
#include <mutex>
#include "../include/gui/Surface.h"

namespace android {

namespace libguiextension {

class QtiSurfaceExtensionGPP {
public:
    QtiSurfaceExtensionGPP(Surface* surface, const sp<IBinder> handle, sp<IGraphicBufferProducer>* gbp);
    bool Connect(int api, sp<IGraphicBufferProducer>* gbp);
    void Disconnect(int api, sp<IGraphicBufferProducer>* gbp);
    bool DynamicEnable(sp<IGraphicBufferProducer>* gbp);
    void StoreConnect(int api, const sp<IProducerListener>& listener, bool producerControlledByApp);
    inline bool IsGPPEnabled() const {
        std::lock_guard lock(mMutex);
        return mIsEnable;
    }
    inline bool IsGPPSupported() const {
        std::lock_guard lock(mMutex);
        return mIsSupported && mConnectedToGpu;
    }
    int getUid() const { return mUID; }
    int query(int what, int* outValue) const;
    void setBufferCount(int bufferCount) {
        std::lock_guard lock(mMutex);
        mClientSetBufferCount = bufferCount;
    }
    void setFrameRate(float frameRate, int8_t compatibility, int8_t changeFrameRateStrategy);
    void setQueuedBufferSlot(int slot) {
        std::lock_guard lock(mMutex);
        mLastQueuedBufferSlot = slot;
    }
    void setAutoPrerotation(bool autoPrerotation) {
        std::lock_guard lock(mMutex);
        mAutoPrerotation = autoPrerotation;
    }
    void setPresentMode(int32_t mode);

    struct SidebandStream {
       bool seted = false;
       sp<NativeHandle> stream = nullptr;
    };
    inline void setSidebandStream(const sp<NativeHandle>& stream) {
       std::lock_guard lock(mMutex);
       mSidebandStream.seted = true;
       mSidebandStream.stream = stream;
    }
    ~QtiSurfaceExtensionGPP();
private:
    bool mIsEnable = false;
    Surface* mSurface;
    bool mIsSupported;
    int mLastEnableProperty = -1;
    bool mConnectedToGpu;
    int mUID = 0;
    int mClientSetBufferCount = 0;
    float mFrameRate = 0.0f;
    int8_t mCompatibility = 0;
    int8_t mChangeFrameRateStrategy = 0;
    int mSessionConflictRetryCount;

    nsecs_t mLastSessionConflictRetryTimestamp;

    sp<IGraphicBufferProducer> mOriginalGbp;
    sp<IGraphicBufferProducer> mGbp;
    sp<IBinder> mHandle;
    void* mLibHandler;
    void* mFuncInit;
    void* mFuncDeinit;
    SidebandStream mSidebandStream;
    int mAPI = 0;
    sp<IProducerListener> mConnectedProducerListener;
    bool mProducerControlledByApp = false;
    mutable std::mutex mMutex;
    int mLastQueuedBufferSlot;
    bool mAutoPrerotation;
    int32_t mPresentMode;
    void DisableGPPinternal(sp<IGraphicBufferProducer>* gbp);
    bool DynamicEnableInternal(sp<IGraphicBufferProducer>* gbp, bool needReconnect);
    void SetGraphicBufferProducer(sp<IGraphicBufferProducer> gbp);
    void TransferBuffersToNewQueue(sp<IGraphicBufferProducer>* gbp);
    bool IsSessionConflictRetryAllowed() const;
};

} // namespace libguiextension
} // namespace android

