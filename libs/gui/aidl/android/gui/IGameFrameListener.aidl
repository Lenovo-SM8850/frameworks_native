// SPDX-License-Identifier: Apache-2.0
package android.gui;

import android.gui.GameFrameStats;

/** Private presentation feedback for the system session authority. @hide */
oneway interface IGameFrameListener {
    void onFrameReported(in GameFrameStats stats);
}
