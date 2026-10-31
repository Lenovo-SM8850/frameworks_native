// SPDX-License-Identifier: Apache-2.0
package android.gui;

/** Private, single-layer presentation feedback. @hide */
parcelable GameFrameStats {
    float fps;
    int appJankFrames;
    int totalFrames;
    int windowMs = 500;
    int layerId = -1;
    boolean layerChanged;
    boolean stalled;
    int appDeadlineMissed;
    int bufferStuffing;
    int predictionErrors;
    int compositorJankFrames;
    // Longest interval between consecutive presents in the window; -1 with fewer than two.
    int maxFrameTimeUs = -1;
    // Highest explicit frame rate vote among the task's visible layers; -1 when none.
    float requestedFps = -1.0f;
    // Frame rate override of the game's uid from the game mode intervention; -1 when none.
    float throttleFps = -1.0f;
}
