// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include "Scheduler/GameFrameTracker.h"

namespace android::scheduler {
namespace {
constexpr int64_t ms = 1000000;

TEST(GameFrameTrackerTest, CountsOnlyPresentedBuffersAndExcludesOverlayFrames) {
    GameFrameTracker tracker;
    for (int i = 1; i <= 60; ++i) {
        tracker.add(1, 1000 * ms + i * (500 * ms / 60), true, true);
        tracker.add(2, 1000 * ms + i * (500 * ms / 60), false, true);
        tracker.add(4, 1000 * ms + i * (500 * ms / 60), true, false);
        if (i % 3 == 0) tracker.add(3, 1000 * ms + i * (500 * ms / 60), true, true);
    }
    auto report = tracker.report({1, 2, 3, 4}, 1500 * ms);
    EXPECT_EQ(1, report.layerId);
    EXPECT_EQ(60, report.totalFrames);
    EXPECT_FLOAT_EQ(120, report.fps);
    EXPECT_TRUE(report.layerChanged);
    EXPECT_FALSE(tracker.report({1, 2, 3, 4}, 1501 * ms).layerChanged);
}

TEST(GameFrameTrackerTest, HiddenProducingBuffersDoNotWinAndHiddenSelectionIsRetired) {
    GameFrameTracker tracker;
    for (int i = 1; i <= 60; ++i) {
        tracker.add(1, (500 + i * 8) * ms, true, true, false, false, false, false, false);
        if (i % 2 == 0) tracker.add(2, (500 + i * 8) * ms, true, true);
    }
    EXPECT_EQ(2, tracker.report({1, 2}, 1000 * ms).layerId);
    tracker.add(3, 1010 * ms, true, true);
    // The composited visible-region set excludes layer 2 as soon as it is hidden.
    const auto report = tracker.report({3}, 1010 * ms);
    EXPECT_EQ(3, report.layerId);
    EXPECT_TRUE(report.layerChanged);
}

TEST(GameFrameTrackerTest, WindowEndsAtDispatchAndAgesOutJankDuringPartialStall) {
    GameFrameTracker tracker;
    for (int i = 1; i <= 60; ++i)
        tracker.add(1, i * (1000 * ms / 60), true, true, i == 45);
    const auto report = tracker.report({1}, 1400 * ms);
    EXPECT_EQ(6, report.totalFrames);
    EXPECT_FLOAT_EQ(12, report.fps);
    EXPECT_EQ(0, report.appJankFrames);
    EXPECT_FALSE(report.stalled);
}

TEST(GameFrameTrackerTest, TimeWindowIsIndependentOfThe64DisplayFrameHistory) {
    GameFrameTracker tracker;
    for (int i = 1; i <= 72; ++i)
        tracker.add(9, 1000 * ms + i * (500 * ms / 72), true, true);
    auto report = tracker.report({9}, 1500 * ms);
    EXPECT_EQ(72, report.totalFrames);
    EXPECT_FLOAT_EQ(144, report.fps);
    EXPECT_EQ(500, report.windowMs);
}

TEST(GameFrameTrackerTest, DetectsIdleStallEvenWhenOverlayContinues) {
    GameFrameTracker tracker;
    for (int i = 1; i <= 50; ++i) tracker.add(1, (1000 + i * 10) * ms, true, true);
    tracker.add(2, 1500 * ms, true, true);
    // Foreign-UID overlays are excluded by the reporter's candidate set.
    EXPECT_EQ(1, tracker.report({1}, 1500 * ms).layerId);
    for (int i = 1; i <= 100; ++i) tracker.add(2, (1500 + i * 5) * ms, true, true);
    auto stalled = tracker.report({1}, 2000 * ms);
    EXPECT_TRUE(stalled.stalled);
    EXPECT_FLOAT_EQ(0, stalled.fps);
    EXPECT_EQ(0, stalled.totalFrames);
    EXPECT_EQ(1, stalled.layerId);
    auto replaced = tracker.report({2}, 2000 * ms);
    EXPECT_TRUE(replaced.layerChanged);
    EXPECT_FALSE(replaced.stalled);
    EXPECT_EQ(2, replaced.layerId);
    EXPECT_FLOAT_EQ(200, replaced.fps);
}

TEST(GameFrameTrackerTest, ReselectsDominantBufferWithoutWaitingForOldUiLayerDestruction) {
    GameFrameTracker tracker;
    for (int i = 1; i <= 50; ++i) tracker.add(1, (1000 + i * 10) * ms, true, true);
    EXPECT_EQ(1, tracker.report({1, 2}, 1500 * ms).layerId);
    for (int i = 1; i <= 100; ++i) tracker.add(2, (1500 + i * 5) * ms, true, true);
    auto report = tracker.report({1, 2}, 2000 * ms);
    EXPECT_EQ(2, report.layerId);
    EXPECT_TRUE(report.layerChanged);
    EXPECT_FLOAT_EQ(200, report.fps);
}

TEST(GameFrameTrackerTest, CountsAppAndCompositorJankSeparately) {
    GameFrameTracker tracker;
    tracker.add(1, 1100 * ms, true, true, true);
    tracker.add(1, 1200 * ms, true, true, false, true);
    tracker.add(1, 1300 * ms, true, true, false, false, true);
    tracker.add(1, 1400 * ms, true, true, false, false, false, true);
    tracker.add(1, 1500 * ms, true, true);
    auto report = tracker.report({1}, 1500 * ms);
    EXPECT_EQ(5, report.totalFrames);
    EXPECT_EQ(2, report.appJankFrames);
    EXPECT_EQ(1, report.appDeadlineMissed);
    EXPECT_EQ(1, report.bufferStuffing);
    EXPECT_EQ(1, report.predictionErrors);
    EXPECT_EQ(1, report.compositorJankFrames);
    tracker.add(1, 1600 * ms, true, true);
    report = tracker.report({1}, 1600 * ms);
    EXPECT_EQ(5, report.totalFrames);
    EXPECT_EQ(1, report.appJankFrames);
}

TEST(GameFrameTrackerTest, ResetAndMissingTaskRetireLayerSelection) {
    GameFrameTracker tracker;
    tracker.add(1, 1000 * ms, true, true);
    EXPECT_EQ(1, tracker.report({1}, 1000 * ms).layerId);
    auto gone = tracker.report({}, 1001 * ms);
    EXPECT_EQ(-1, gone.layerId);
    EXPECT_TRUE(gone.layerChanged);
    EXPECT_TRUE(gone.stalled);
    tracker.reset();
    EXPECT_EQ(0, tracker.report({1}, 1001 * ms).totalFrames);
}

// Establish selection before collecting cadence; only the selected buffer layer
// owns interval history, and layer changes deliberately discard that history.
void addTokenless(GameFrameTracker& tracker, int32_t layer, int64_t present) {
    tracker.add(layer, present, true, true, false, false, false, false, true, false);
}

int64_t warmCadence(GameFrameTracker& tracker, int64_t period = 16 * ms) {
    int64_t present = 1000 * ms;
    addTokenless(tracker, 1, present);
    EXPECT_EQ(1, tracker.report({1}, present).layerId);
    for (int i = 0; i < 10; ++i) addTokenless(tracker, 1, present += period);
    return present;
}

TEST(GameFrameTrackerTest, TokenlessSteadyCadenceDoesNotCountDisplayRefreshMultiples) {
    for (auto period : {8 * ms, 16 * ms, 33 * ms}) {
        GameFrameTracker tracker;
        const auto present = warmCadence(tracker, period);
        const auto report = tracker.report({1}, present);
        EXPECT_EQ(0, report.appJankFrames);
        EXPECT_EQ(0, report.cadenceJankFrames);
        EXPECT_EQ(report.totalFrames, report.cadenceFrames);
        EXPECT_EQ(0, report.classifiedFrames);
    }
}

TEST(GameFrameTrackerTest, TokenlessSingleLongFrameCountsOneJank) {
    GameFrameTracker tracker;
    auto present = warmCadence(tracker);
    addTokenless(tracker, 1, present += 40 * ms);
    addTokenless(tracker, 1, present += 16 * ms);
    const auto report = tracker.report({1}, present);
    EXPECT_EQ(1, report.appJankFrames);
    EXPECT_EQ(1, report.cadenceJankFrames);
    EXPECT_EQ(0, report.appDeadlineMissed);
    EXPECT_EQ(0, report.bufferStuffing);
}

TEST(GameFrameTrackerTest, TokenlessTwoVsyncIntervalCountsOneMissedFrame) {
    GameFrameTracker tracker;
    auto present = warmCadence(tracker, 8 * ms);
    addTokenless(tracker, 1, present += 16 * ms);
    EXPECT_EQ(1, tracker.report({1}, present).appJankFrames);
    // A single outlier must not poison the reference for a later long frame.
    for (int i = 0; i < 2; ++i) addTokenless(tracker, 1, present += 8 * ms);
    addTokenless(tracker, 1, present += 16 * ms);
    EXPECT_EQ(2, tracker.report({1}, present).appJankFrames);
}

TEST(GameFrameTrackerTest, TokenlessSixtyToThirtyCadenceChangeDoesNotStorm) {
    GameFrameTracker tracker;
    constexpr int64_t period = 1000000000 / 60;
    auto present = warmCadence(tracker, period);
    for (int i = 0; i < 20; ++i) {
        addTokenless(tracker, 1, present += 2 * period);
        EXPECT_LE(tracker.report({1}, present).appJankFrames, 1);
    }
    // Only the initial indistinguishable long interval can have been counted;
    // it ages out instead of generating repeated boosts at the new target.
    EXPECT_EQ(0, tracker.report({1}, present).appJankFrames);
    for (int i = 0; i < 10; ++i) addTokenless(tracker, 1, present += period);
    EXPECT_EQ(0, tracker.report({1}, present).appJankFrames);
}

TEST(GameFrameTrackerTest, TokenlessStallThenResumeDoesNotBurst) {
    GameFrameTracker tracker;
    auto present = warmCadence(tracker);
    EXPECT_TRUE(tracker.report({1}, present + GameFrameTracker::kWindowNs).stalled);
    addTokenless(tracker, 1, present += 800 * ms);
    for (int i = 0; i < 10; ++i) addTokenless(tracker, 1, present += 33 * ms);
    EXPECT_EQ(0, tracker.report({1}, present).appJankFrames);
    // A long gap also resets cadence when no timer report occurred in the gap.
    addTokenless(tracker, 1, present += 800 * ms);
    EXPECT_EQ(0, tracker.report({1}, present).appJankFrames);
}

TEST(GameFrameTrackerTest, TokenBearingFramesUseClassificationExclusively) {
    GameFrameTracker tracker;
    auto present = warmCadence(tracker);
    tracker.add(1, present += 32 * ms, true, true);
    auto report = tracker.report({1}, present);
    EXPECT_EQ(0, report.appJankFrames);
    EXPECT_EQ(1, report.classifiedFrames);
    // A classified app miss counts even though this interval is on cadence.
    tracker.add(1, present += 16 * ms, true, true, true);
    report = tracker.report({1}, present);
    EXPECT_EQ(1, report.appJankFrames);
    EXPECT_EQ(0, report.cadenceJankFrames);
    EXPECT_EQ(2, report.classifiedFrames);
    // Returning to tokenless presentation requires fresh cadence history.
    addTokenless(tracker, 1, present += 60 * ms);
    EXPECT_EQ(1, tracker.report({1}, present).appJankFrames);
}

TEST(GameFrameTrackerTest, TokenlessLayerChangeResetsCadence) {
    GameFrameTracker tracker;
    auto present = warmCadence(tracker, 8 * ms);
    addTokenless(tracker, 2, present += 16 * ms);
    EXPECT_EQ(2, tracker.report({2}, present).layerId);
    for (int i = 0; i < 10; ++i) addTokenless(tracker, 2, present += 33 * ms);
    const auto report = tracker.report({2}, present);
    EXPECT_EQ(0, report.appJankFrames);
    EXPECT_EQ(report.totalFrames, report.cadenceFrames);
}

TEST(GameFrameTrackerTest, TokenlessShortOutlierDoesNotLowerReference) {
    GameFrameTracker tracker;
    auto present = warmCadence(tracker);
    addTokenless(tracker, 1, present += 8 * ms);
    addTokenless(tracker, 1, present += 16 * ms);
    EXPECT_EQ(0, tracker.report({1}, present).appJankFrames);
}

TEST(GameFrameTrackerTest, TokenlessVsyncQuantizationRemovesFenceNoiseAtThreshold) {
    GameFrameTracker tracker;
    int64_t present = 1000 * ms;
    const auto add = [&](int64_t interval, int64_t vsync) {
        tracker.add(1, present += interval, true, true, false, false, false, false, true, false,
                    vsync);
    };
    add(0, 8 * ms);
    EXPECT_EQ(1, tracker.report({1}, present).layerId);
    for (int i = 0; i < 10; ++i) add(16 * ms + (i % 2 ? 10000 : -10000), 8 * ms);
    add(24 * ms + 10000, 8 * ms);
    EXPECT_EQ(0, tracker.report({1}, present).appJankFrames);
    add(32 * ms, 8 * ms);
    EXPECT_EQ(1, tracker.report({1}, present).appJankFrames);
    // Refresh-rate changes discard the old quantum and warm up again.
    add(48 * ms, 16 * ms);
    EXPECT_EQ(1, tracker.report({1}, present).appJankFrames);
}
} // namespace
} // namespace android::scheduler
