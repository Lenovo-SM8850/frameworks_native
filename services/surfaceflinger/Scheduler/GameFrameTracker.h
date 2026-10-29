// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <unordered_set>

namespace android::scheduler {

// Independent of the display-frame count limit, so high refresh rates retain a
// complete time window ending at dispatch. Timestamps use the monotonic clock.
class GameFrameTracker {
public:
    static constexpr int64_t kWindowNs = 500000000;
    struct Report {
        float fps = 0;
        int32_t appJankFrames = 0, totalFrames = 0, windowMs = 500, layerId = -1;
        int32_t appDeadlineMissed = 0, bufferStuffing = 0, predictionErrors = 0;
        int32_t compositorJankFrames = 0;
        // Provenance stays local to SF; no wire/API change is needed for feedback.
        int32_t classifiedFrames = 0, cadenceFrames = 0, cadenceJankFrames = 0;
        bool layerChanged = false, stalled = true;
    };

    void reset() { mFrames.clear(); mLayer = -1; mLastPresent = mNewest = 0; resetCadence(); }

    void add(int32_t layer, int64_t present, bool buffer, bool presented,
             bool appDeadline = false, bool stuffing = false, bool prediction = false,
             bool compositor = false, bool visibleAtComposition = true, bool hasPrediction = true,
             int64_t displayPeriod = 0) {
        if (!buffer || !presented || !visibleAtComposition || present <= 0) return;
        mNewest = std::max(mNewest, present);
        if (present <= mNewest - 2 * kWindowNs) return;
        bool cadenceJank = false;
        if (layer == mLayer) {
            if (hasPrediction) resetCadence();
            else cadenceJank = sampleCadence(present, displayPeriod);
        }
        mFrames.push_back({layer, present, appDeadline, stuffing, prediction, compositor,
                           hasPrediction, cadenceJank});
        // Retain fence-completion headroom; reports still filter at dispatch time.
        while (!mFrames.empty() && mFrames.front().present <= mNewest - 2 * kWindowNs)
            mFrames.pop_front();
        if (layer == mLayer) mLastPresent = std::max(mLastPresent, present);
    }

    Report report(const std::unordered_set<int32_t>& candidates, int64_t now) {
        const auto previous = mLayer;
        if (!candidates.count(mLayer)) { mLayer = -1; mLastPresent = 0; }
        int64_t latest = 0;
        for (const auto& frame : mFrames) if (candidates.count(frame.layer))
            latest = std::max(latest, frame.present);
        if (latest > 0 && now - latest < kWindowNs) {
            std::unordered_map<int32_t, int> counts;
            for (const auto& frame : mFrames)
                if (candidates.count(frame.layer) && frame.present > now - kWindowNs && frame.present <= now)
                    ++counts[frame.layer];
            int best = 0;
            for (const auto& [layer, count] : counts) {
                if (count > best || (count == best && (layer == previous ||
                        (mLayer != previous && layer < mLayer)))) { best = count; mLayer = layer; }
            }
        }
        if (previous != mLayer) { mLastPresent = 0; resetCadence(); }
        for (const auto& frame : mFrames) if (frame.layer == mLayer)
            mLastPresent = std::max(mLastPresent, frame.present);
        Report result;
        result.layerId = mLayer;
        result.layerChanged = previous != mLayer;
        result.stalled = mLastPresent <= 0 || now - mLastPresent >= kWindowNs;
        if (result.stalled) { resetCadence(); return result; }
        for (const auto& frame : mFrames) {
            if (frame.layer != mLayer || frame.present <= now - kWindowNs || frame.present > now) continue;
            ++result.totalFrames;
            result.appDeadlineMissed += frame.appDeadline;
            result.bufferStuffing += frame.stuffing;
            result.predictionErrors += frame.prediction;
            result.compositorJankFrames += frame.compositor;
            result.classifiedFrames += frame.hasPrediction;
            result.cadenceFrames += !frame.hasPrediction;
            result.cadenceJankFrames += frame.cadenceJank;
            // Token-bearing frames use classification exclusively. Otherwise count
            // one late presented frame per interval exceeding 1.5 times cadence.
            result.appJankFrames += frame.hasPrediction ? frame.appDeadline || frame.stuffing
                                                       : frame.cadenceJank;
        }
        result.fps = result.totalFrames * 1000.0f / result.windowMs;
        return result;
    }

private:
    struct Frame {
        int32_t layer;
        int64_t present;
        bool appDeadline, stuffing, prediction, compositor, hasPrediction, cadenceJank;
    };
    void resetCadence() {
        mCadenceLastPresent = mPreviousLongInterval = mCadencePeriod = 0;
        mIntervalCount = mIntervalNext = 0;
    }

    bool sampleCadence(int64_t present, int64_t displayPeriod) {
        if (displayPeriod > 0 && (mCadencePeriod == 0 ||
            displayPeriod < mCadencePeriod - mCadencePeriod / 20 ||
            displayPeriod > mCadencePeriod + mCadencePeriod / 20)) {
            resetCadence();
            mCadencePeriod = displayPeriod;
        }
        auto interval = present - mCadenceLastPresent;
        const bool warmup = mCadenceLastPresent <= 0 || interval <= 0 || interval >= kWindowNs;
        mCadenceLastPresent = present;
        if (warmup) {
            mIntervalCount = mIntervalNext = 0;
            mPreviousLongInterval = 0;
            return false;
        }
        // Use one stable quantum until refresh changes by more than 5%. This
        // removes fence noise at the 1.5-cadence boundary without making small
        // refresh-estimate fluctuations reset history or change the threshold.
        if (mCadencePeriod > 0) {
            interval = std::max<int64_t>(1, (interval + mCadencePeriod / 2) / mCadencePeriod) *
                    mCadencePeriod;
        }
        bool late = false;
        if (mIntervalCount == mIntervals.size()) {
            auto sorted = mIntervals;
            std::sort(sorted.begin(), sorted.end());
            const auto cadence = sorted[sorted.size() / 2];
            late = interval > cadence + cadence / 2;
            if (late && mPreviousLongInterval > 0 &&
                interval >= mPreviousLongInterval - cadence / 4 &&
                interval <= mPreviousLongInterval + cadence / 4) {
                // Two similar long intervals establish a slower target. Faster
                // targets settle through the median: one short interval must not
                // lower the reference. At most the first slower interval is
                // counted, since it is indistinguishable from an isolated miss.
                mIntervals.fill(interval);
                mPreviousLongInterval = 0;
                return false;
            }
            mPreviousLongInterval = late ? interval : 0;
        }
        mIntervals[mIntervalNext] = interval;
        mIntervalNext = (mIntervalNext + 1) % mIntervals.size();
        mIntervalCount = std::min(mIntervalCount + 1, mIntervals.size());
        return late;
    }

    // Only the selected buffer layer owns cadence history. Five previous
    // intervals use a median rather than an average so one
    // long frame cannot raise the baseline. Fixed size makes the sort O(1).
    std::array<int64_t, 5> mIntervals{};
    size_t mIntervalCount = 0, mIntervalNext = 0;
    int64_t mCadenceLastPresent = 0, mPreviousLongInterval = 0, mCadencePeriod = 0;
    std::deque<Frame> mFrames;
    int32_t mLayer = -1;
    int64_t mLastPresent = 0, mNewest = 0;
};
} // namespace android::scheduler
