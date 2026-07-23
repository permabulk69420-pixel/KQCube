// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.activities

import android.content.Context
import android.os.Build
import android.os.Bundle
import java.util.Locale
import org.dolphinemu.dolphinemu.NativeLibrary

/**
 * Thin Quest lifecycle wrapper around Dolphin's stock emulation activity.
 *
 * All UI, game boot, audio, and fallback presentation remain owned by [EmulationActivity]. The
 * native OpenXR presenter is requested on Quest hardware even when Android omits the advisory VR
 * head-tracking PackageManager feature.
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

        fun isSupported(context: Context): Boolean {
            if (context.packageManager.hasSystemFeature(FEATURE_VR_HEADTRACKING))
                return true

            // Quest firmware can expose a working OpenXR runtime while omitting the advisory
            // android.hardware.vr.headtracking feature. Match the proven KQ-64 launch gate instead
            // of incorrectly routing those headsets into stock flat Dolphin.
            val manufacturer = Build.MANUFACTURER.orEmpty().lowercase(Locale.US)
            val model = Build.MODEL.orEmpty().lowercase(Locale.US)
            return manufacturer == "oculus" || manufacturer == "meta" || model.contains("quest")
        }
    }
}
