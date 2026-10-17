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
        fun event(name: String, level: String = "INFO", fields: () -> Map<String, String>) = Diagnostics.fieldsForSession(session, name, level, fields)
        if (Build.VERSION.SDK_INT < 30) {
            event("PROCESS_EXIT_HISTORY_UNAVAILABLE") { mapOf("reason" to "android_api_below_30") }
            return
        }
        runCatching {
            val ownUid = context.applicationInfo.uid
            val exits = queryBounded(1500) {
                val manager = context.getSystemService(ActivityManager::class.java)
                manager.getHistoricalProcessExitReasons(context.packageName, 0, 4).take(4)
                    .filter { it.realUid == ownUid && (it.processName == context.packageName || it.processName == context.packageName + ":emulationProcess") }
                    .sortedByDescending { it.timestamp }
            }
            event("PROCESS_EXIT_HISTORY_QUERIED") { mapOf("source" to "Android_ApplicationExitInfo", "records" to exits.size.toString(), "scope" to "own_package_and_uid") }
            ExitRecordStore(Diagnostics.rootDirectory(context)).locked { seen ->
                var traceAttempted = false
                exits.forEach { exit ->
                    val scope = if (exit.processName.endsWith(":emulationProcess")) "emulation" else "main"
                    val id = ExitRecordIdentity.id(exit.timestamp, exit.reason, exit.status, exit.pid, scope)
                    val decoded = ProcessAssociationCodec.decode(exit.processStateSummary)
                    val association = decoded?.takeIf { it.scope == scope && it.process.isNotEmpty() && it.session.isNotEmpty() }
                    val relation = if (association != null) "anonymous_process_state" else "unknown_or_unavailable"
                    fun previous() = mapOf("previous_session_id" to association?.session.orEmpty(), "previous_launch_id" to association?.launch.orEmpty(),
                        "previous_process_run_id" to association?.process.orEmpty(), "previous_commit" to association?.commit.orEmpty())
                    if ("e:$id" !in seen) {
                        val accepted = event(eventName(exit.reason), if (eventName(exit.reason) == "PREVIOUS_PROCESS_CRASH") "ERROR" else "INFO") {
                            mapOf("source" to "Android_ApplicationExitInfo", "historical_record" to "true", "exit_record_id" to id,
                                "process_scope" to scope, "reason_code" to exit.reason.toString(), "status" to exit.status.toString(),
                                "exit_wall_clock_ms" to exit.timestamp.toString(), "association_status" to relation,
                                "previous_application_title_id" to association?.title.orEmpty(), "previous_diagnostics_enabled" to (association?.enabled?.toString() ?: "unknown"),
                                "previous_launch_start_observed" to (association?.startObserved?.toString() ?: "unknown")) + previous()
                        }
                        if (accepted && Diagnostics.flushForSession(session)) seen.add("e:$id")
                    }
                    if (exit.reason == ApplicationExitInfo.REASON_CRASH_NATIVE && !traceAttempted && "t:$id" !in seen) {
                        traceAttempted = true
                        val trace = if (Build.VERSION.SDK_INT < 31) NativeTraceSummary.Result("android_api_below_31")
                        else try {
                            queryBounded(1500) { NativeTraceSummary.read(exit.traceInputStream, exit.pid, ownUid) }
                        } catch (_: java.util.concurrent.TimeoutException) { NativeTraceSummary.Result("observer_deadline") }
                        catch (_: Exception) { NativeTraceSummary.Result("read_unavailable") }
                        val accepted = event("PREVIOUS_NATIVE_TRACE") {
                            mapOf("source" to "Android_ApplicationExitInfo", "historical_record" to "true", "exit_record_id" to id,
                                "association_status" to relation) + previous() + trace.fields()
                        }
                        if (accepted && Diagnostics.flushForSession(session)) seen.add("t:$id")
                    }
                }
            }
        }.onFailure { error ->
            event("PROCESS_EXIT_HISTORY_UNAVAILABLE") { mapOf("reason" to error.javaClass.simpleName, "scope" to "query_or_private_dedup_store") }
        }
    }
}
