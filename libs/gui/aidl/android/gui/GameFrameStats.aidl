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
}
