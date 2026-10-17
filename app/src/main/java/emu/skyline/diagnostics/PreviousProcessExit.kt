// SPDX-License-Identifier: MPL-2.0
package emu.skyline.diagnostics

import android.app.ActivityManager
import android.app.ApplicationExitInfo
import android.content.Context
import android.os.Build
import java.util.concurrent.Callable
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

/** System observations after restart; never a claim about the new session's game. */
internal object PreviousProcessExit {
    internal fun <T> queryBounded(timeoutMilliseconds: Long, operation: () -> T): T {
        val executor = Executors.newSingleThreadExecutor { job ->
            Thread(job, "PineExitQuery").apply { isDaemon = true }
        }
        val future = executor.submit(Callable { operation() })
        try {
            return future.get(timeoutMilliseconds, TimeUnit.MILLISECONDS)
        } finally {
            future.cancel(true)
            executor.shutdownNow()
        }
    }
    internal fun eventName(reason: Int): String = when (reason) {
        ApplicationExitInfo.REASON_CRASH, ApplicationExitInfo.REASON_CRASH_NATIVE,
        ApplicationExitInfo.REASON_ANR -> "PREVIOUS_PROCESS_CRASH"
        else -> "PREVIOUS_PROCESS_EXIT"
    }

    fun schedule(context: Context, session: String) {
        // Only one query is scheduled per process by Diagnostics.apply. Neither
        // the UI nor emulation/session shutdown waits for this daemon observer.
        Thread({ observe(context, session) }, "PineExitObserver").apply { isDaemon = true }.start()
    }

    private fun observe(context: Context, session: String) {
        fun event(name: String, detail: () -> String) = Diagnostics.eventForSession(session, "lifecycle", name, detail)
        if (Build.VERSION.SDK_INT < 30) {
            event("PROCESS_EXIT_HISTORY_UNAVAILABLE") { "reason=android_api_below_30" }
            return
        }
        runCatching {
            val exits = queryBounded(1500) {
                val manager = context.getSystemService(ActivityManager::class.java)
                manager.getHistoricalProcessExitReasons(context.packageName, 0, 4).take(4)
                    .filter { it.processName == context.packageName || it.processName == context.packageName + ":emulationProcess" }
            }
            event("PROCESS_EXIT_HISTORY_QUERIED") { "source=Android_ApplicationExitInfo records=${exits.size} scope=own_package historical_session_build_title=unknown" }
            exits.forEach { exit ->
                event(eventName(exit.reason)) {
                    "source=Android_ApplicationExitInfo historical_record=true previous_session_build_title=unknown " +
                        "process_scope=${if (exit.processName.endsWith(":emulationProcess")) "emulation" else "main"} " +
                        "reason_code=${exit.reason} status=${exit.status} exit_wall_clock_ms=${exit.timestamp} " +
                        "native_stack=not_collected"
                }
            }
        }.onFailure { error ->
            event("PROCESS_EXIT_HISTORY_UNAVAILABLE") { "reason=${error.javaClass.simpleName}" }
        }
    }
}
