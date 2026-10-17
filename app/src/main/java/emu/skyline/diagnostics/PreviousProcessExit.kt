// SPDX-License-Identifier: MPL-2.0
package emu.skyline.diagnostics

import android.app.ActivityManager
import android.app.ApplicationExitInfo
import android.content.Context
import android.os.Build

/** System observations after restart; never a claim about the new session's game. */
internal object PreviousProcessExit {
    internal fun eventName(reason: Int): String = when (reason) {
        ApplicationExitInfo.REASON_CRASH, ApplicationExitInfo.REASON_CRASH_NATIVE,
        ApplicationExitInfo.REASON_ANR -> "PREVIOUS_PROCESS_CRASH"
        else -> "PREVIOUS_PROCESS_EXIT"
    }

    fun observe(context: Context) {
        if (Build.VERSION.SDK_INT < 30) {
            Diagnostics.event("lifecycle", "PROCESS_EXIT_HISTORY_UNAVAILABLE") { "reason=android_api_below_30" }
            return
        }
        runCatching {
            val manager = context.getSystemService(ActivityManager::class.java)
            val exits = manager.getHistoricalProcessExitReasons(context.packageName, 0, 4)
                .filter { it.processName == context.packageName || it.processName == context.packageName + ":emulationProcess" }
            Diagnostics.event("lifecycle", "PROCESS_EXIT_HISTORY_QUERIED") { "source=Android_ApplicationExitInfo records=${exits.size} scope=own_package historical_session_build_title=unknown" }
            exits.forEach { exit ->
                Diagnostics.event("lifecycle", eventName(exit.reason)) {
                    "source=Android_ApplicationExitInfo historical_record=true previous_session_build_title=unknown " +
                        "process_scope=${if (exit.processName.endsWith(":emulationProcess")) "emulation" else "main"} " +
                        "reason_code=${exit.reason} status=${exit.status} exit_wall_clock_ms=${exit.timestamp} " +
                        "native_stack=not_collected"
                }
            }
        }.onFailure { error ->
            Diagnostics.event("lifecycle", "PROCESS_EXIT_HISTORY_UNAVAILABLE") { "reason=${error.javaClass.simpleName}" }
        }
    }
}
