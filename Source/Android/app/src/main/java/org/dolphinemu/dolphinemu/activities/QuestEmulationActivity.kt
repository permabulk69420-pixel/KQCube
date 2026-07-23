// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.activities

import android.content.Context
import android.os.Bundle
import org.dolphinemu.dolphinemu.NativeLibrary

/**
 * Thin Quest lifecycle wrapper around Dolphin's stock emulation activity.
 *
 * All UI, game boot, audio, and fallback presentation remain owned by [EmulationActivity]. The
 * native OpenXR presenter is only requested on devices that advertise Android VR head tracking.
 */
class QuestEmulationActivity : EmulationActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        NativeLibrary.InitializeOpenXR(this)
        super.onCreate(savedInstanceState)
    }

    override fun onDestroy() {
        NativeLibrary.ShutdownOpenXR(this)
        super.onDestroy()
    }

    companion object {
        private const val FEATURE_VR_HEADTRACKING = "android.hardware.vr.headtracking"

        fun isSupported(context: Context): Boolean =
            context.packageManager.hasSystemFeature(FEATURE_VR_HEADTRACKING)
    }
}
