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

#include <algorithm>

#include "FpsReporter.h"
#include "Layer.h"
#include "SurfaceFlinger.h"

namespace android {

FpsReporter::FpsReporter(scheduler::FrameTimeline& frameTimeline, std::unique_ptr<Clock> clock)
      : mFrameTimeline(frameTimeline), mClock(std::move(clock)) {
    LOG_ALWAYS_FATAL_IF(mClock == nullptr, "Passed in null clock when constructing FpsReporter!");
}

void FpsReporter::dispatchLayerFps(const frontend::LayerHierarchy& layerHierarchy) {
    const auto now = mClock->now();
    if (now - mLastDispatch < kMinDispatchDuration) {
        return;
    }

    std::vector<TrackedListener> localListeners;
    {
        std::scoped_lock lock(mMutex);
        if (mListeners.empty()) {
            return;
        }

        std::transform(mListeners.begin(), mListeners.end(), std::back_inserter(localListeners),
                       [](const std::pair<wp<IBinder>, TrackedListener>& entry) {
                           return entry.second;
                       });
    }

    std::unordered_set<int32_t> seenTasks;
    std::vector<std::pair<TrackedListener, const frontend::LayerHierarchy*>>
            listenersAndLayersToReport;

    layerHierarchy.traverse([&](const frontend::LayerHierarchy& hierarchy,
                                const frontend::LayerHierarchy::TraversalPath& traversalPath) {
        if (traversalPath.variant == frontend::LayerHierarchy::Variant::Detached) {
            return false;
        }
        const auto& metadata = hierarchy.getLayer()->metadata;
        if (metadata.has(gui::METADATA_TASK_ID)) {
            int32_t taskId = metadata.getInt32(gui::METADATA_TASK_ID, 0);
            if (seenTasks.count(taskId) == 0) {
                // localListeners is expected to be tiny
                for (TrackedListener& listener : localListeners) {
                    if (listener.taskId == taskId) {
                        seenTasks.insert(taskId);
                        listenersAndLayersToReport.push_back({listener, &hierarchy});
                        break;
                    }
                }
            }
        }
        return true;
    });

    for (const auto& [listener, hierarchy] : listenersAndLayersToReport) {
        std::unordered_set<int32_t> layerIds;

        hierarchy->traverse([&](const frontend::LayerHierarchy& hierarchy,
                                const frontend::LayerHierarchy::TraversalPath& traversalPath) {
            if (traversalPath.variant == frontend::LayerHierarchy::Variant::Detached) {
                return false;
            }
            layerIds.insert(static_cast<int32_t>(hierarchy.getLayer()->id));
            return true;
        });

        listener.listener->onFpsReported(mFrameTimeline.computeFps(layerIds));
    }

    mLastDispatch = now;
}

void FpsReporter::binderDied(const wp<IBinder>& who) {
    std::scoped_lock lock(mMutex);
    mListeners.erase(who);
    if (mGameListener && wp<IBinder>(IInterface::asBinder(mGameListener->listener)) == who) {
        mGameListener.reset(); ++mGameGeneration;
        mFrameTimeline.setGameFrameTracking(false, -1);
    }
}

void FpsReporter::addListener(const sp<gui::IFpsListener>& listener, int32_t taskId) {
    sp<IBinder> asBinder = IInterface::asBinder(listener);
    asBinder->linkToDeath(sp<DeathRecipient>::fromExisting(this));
    std::lock_guard lock(mMutex);
    mListeners.emplace(wp<IBinder>(asBinder), TrackedListener{listener, taskId});
}

void FpsReporter::removeListener(const sp<gui::IFpsListener>& listener) {
    std::lock_guard lock(mMutex);
    mListeners.erase(wp<IBinder>(IInterface::asBinder(listener)));
}

uint64_t FpsReporter::addGameListener(const sp<gui::IGameFrameListener>& listener,
                                     int32_t taskId, int32_t uid) {
    std::lock_guard lock(mMutex);
    if (mGameListener) return 0;
    auto binder = IInterface::asBinder(listener);
    if (binder->linkToDeath(sp<DeathRecipient>::fromExisting(this)) != OK) return 0;
    mGameListener = GameListener{listener, taskId, uid};
    mGameLogTime = mClock->now();
    mGameLog = {};
    mFrameTimeline.setGameFrameTracking(true, uid);
    return ++mGameGeneration;
}

void FpsReporter::removeGameListener(const sp<gui::IGameFrameListener>& listener) {
    std::lock_guard lock(mMutex);
    if (!mGameListener || IInterface::asBinder(mGameListener->listener) != IInterface::asBinder(listener)) return;
    IInterface::asBinder(listener)->unlinkToDeath(sp<DeathRecipient>::fromExisting(this));
    mGameListener.reset(); ++mGameGeneration;
    mFrameTimeline.setGameFrameTracking(false, -1);
}

bool FpsReporter::dispatchGameFrame(uint64_t generation, const frontend::LayerHierarchy& hierarchy) {
    sp<gui::IGameFrameListener> listener;
    scheduler::GameFrameTracker::Report report;
    {
        std::lock_guard lock(mMutex);
        if (!mGameListener || generation != mGameGeneration) return false;
        const auto tracked = *mGameListener;
        std::unordered_set<int32_t> layers;
        hierarchy.traverse([&](const frontend::LayerHierarchy& node,
                               const frontend::LayerHierarchy::TraversalPath& path) {
            if (path.variant == frontend::LayerHierarchy::Variant::Detached) return false;
            const auto& metadata = node.getLayer()->metadata;
            if (!metadata.has(gui::METADATA_TASK_ID) ||
                metadata.getInt32(gui::METADATA_TASK_ID, -1) != tracked.taskId) return true;
            node.traverse([&](const frontend::LayerHierarchy& child,
                              const frontend::LayerHierarchy::TraversalPath& childPath) {
                if (childPath.variant == frontend::LayerHierarchy::Variant::Detached) return false;
                const auto& childMetadata = child.getLayer()->metadata;
                if (childMetadata.has(gui::METADATA_TASK_ID) &&
                    childMetadata.getInt32(gui::METADATA_TASK_ID, -1) != tracked.taskId) return false;
                if (child.getLayer()->ownerUid.val() == static_cast<uid_t>(tracked.uid))
                    layers.insert(static_cast<int32_t>(child.getLayer()->id));
                return true;
            });
            return false;
        });
        report = mFrameTimeline.computeGameFrameStats(layers, systemTime(SYSTEM_TIME_MONOTONIC));
        listener = tracked.listener;
        mGameLog.totalFrames += report.totalFrames;
        mGameLog.appJankFrames += report.appJankFrames;
        mGameLog.cadenceJankFrames += report.cadenceJankFrames;
        mGameLog.classifiedFrames += report.classifiedFrames;
        mGameLog.cadenceFrames += report.cadenceFrames;
        const auto now = mClock->now();
        if (now - mGameLogTime >= std::chrono::seconds(30)) {
            ALOGI("Game frame feedback layer=%d frames=%d app_jank=%d classified_frames=%d "
                  "cadence_frames=%d cadence_jank=%d", report.layerId, mGameLog.totalFrames,
                  mGameLog.appJankFrames, mGameLog.classifiedFrames, mGameLog.cadenceFrames,
                  mGameLog.cadenceJankFrames);
            mGameLogTime = now;
            mGameLog = {};
        }
    }
    gui::GameFrameStats stats;
    stats.fps = report.fps;
    stats.appJankFrames = report.appJankFrames; stats.totalFrames = report.totalFrames;
    stats.windowMs = report.windowMs; stats.layerId = report.layerId;
    stats.layerChanged = report.layerChanged; stats.stalled = report.stalled;
    stats.appDeadlineMissed = report.appDeadlineMissed; stats.bufferStuffing = report.bufferStuffing;
    stats.predictionErrors = report.predictionErrors; stats.compositorJankFrames = report.compositorJankFrames;
    listener->onFrameReported(stats);
    std::lock_guard lock(mMutex);
    return mGameListener && generation == mGameGeneration;
}

} // namespace android
