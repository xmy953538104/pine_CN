// SPDX-License-Identifier: MPL-2.0
package emu.skyline.diagnostics

import android.app.ActivityManager
import android.app.ApplicationExitInfo
import android.content.Context
import android.os.Build
import java.io.InputStream
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
    internal fun fatalSignal(reason: Int, status: Int): Boolean =
        reason == ApplicationExitInfo.REASON_SIGNALED && status in setOf(4, 6, 7, 8, 11)

    internal fun traceCandidate(reason: Int, status: Int): Boolean =
        reason == ApplicationExitInfo.REASON_CRASH_NATIVE || fatalSignal(reason, status)

    internal fun eventName(reason: Int, previouslyObserved: Boolean = false, status: Int = 0): String = if (previouslyObserved) "PREVIOUS_PROCESS_EXIT_REFINED" else if (fatalSignal(reason, status)) "PREVIOUS_PROCESS_FATAL_SIGNAL" else when (reason) {
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
            }
            observeRecords(exits, ownUid, context.packageName, Build.VERSION.SDK_INT,
                ExitRecordStore(Diagnostics.rootDirectory(context)),
                publish = { name, level, fields -> event(name, level) { fields } },
                flush = { Diagnostics.flushForSession(session) })
        }.onFailure { error ->
            event("PROCESS_EXIT_HISTORY_UNAVAILABLE") { mapOf("reason" to error.javaClass.simpleName, "scope" to "query_or_private_dedup_store") }
        }
    }

    /** Same bounded production path, with system stream and sink seams for fault tests. */
    internal fun observeRecords(records: List<ApplicationExitInfo>, ownUid: Int, packageName: String,
        apiLevel: Int, store: ExitRecordStore,
        traceInput: (ApplicationExitInfo) -> InputStream? = { it.traceInputStream },
        traceTimeoutMilliseconds: Long = 1500,
        publish: (String, String, Map<String, String>) -> Boolean, flush: () -> Boolean) {
        fun event(name: String, level: String = "INFO", fields: () -> Map<String, String>) = publish(name, level, fields())
        val exits = records.take(4).filter { ownUid > 0 && it.pid > 0 && it.realUid == ownUid &&
            (it.processName == packageName || it.processName == "$packageName:emulationProcess") }
            .sortedByDescending { it.timestamp }
        event("PROCESS_EXIT_HISTORY_QUERIED") { mapOf("source" to "Android_ApplicationExitInfo", "records" to exits.size.toString(), "scope" to "own_package_and_uid") }
        store.locked { seen ->
            // Migrate before eligibility: legacy terminal/retry markers must participate
            // in selection. Native-crash records have the documented tombstone contract;
            // optional signaled candidates must not displace those within our one-read budget.
            val candidates = exits.map { exit ->
                val scope = if (exit.processName.endsWith(":emulationProcess")) "emulation" else "main"
                Triple(exit, scope, ExitRecordIdentity.migrate(seen, exit.timestamp, exit.reason, exit.status, exit.pid, scope))
            }
            val eligible = candidates.filter { (exit, _, id) ->
                traceCandidate(exit.reason, exit.status) && NativeTraceImport.eligible(seen, id)
            }
            val selected = eligible.firstOrNull { it.first.reason == ApplicationExitInfo.REASON_CRASH_NATIVE }
                ?: eligible.firstOrNull()
            var traceAttempted = false
            candidates.forEach { (exit, scope, id) ->
                val association = ProcessAssociationCodec.forScope(exit.processStateSummary, scope)
                val relation = if (association != null) "verified" else "unknown"
                fun previous() = mapOf("previous_session_id" to association?.session.orEmpty(), "previous_launch_id" to association?.launch.orEmpty(),
                    "previous_process_run_id" to association?.process.orEmpty(), "previous_commit" to association?.commit.orEmpty())
                val firstObservation = "e:$id" !in seen
                if (ExitRecordIdentity.needsObservation(seen, id, exit.status)) {
                    val name = eventName(exit.reason, !firstObservation, exit.status)
                    val accepted = event(name, if (name == "PREVIOUS_PROCESS_CRASH" || name == "PREVIOUS_PROCESS_FATAL_SIGNAL") "ERROR" else "INFO") {
                        mapOf("source" to "Android_ApplicationExitInfo", "historical_record" to "true",
                            "exit_classification" to (if (fatalSignal(exit.reason, exit.status)) "historical_fatal_signal" else if (eventName(exit.reason) == "PREVIOUS_PROCESS_CRASH") "historical_crash" else "historical_exit"),
                            "exit_scope" to "host_process", "process_scope" to scope, "reason_code" to exit.reason.toString(), "status" to exit.status.toString(),
                            "exit_wall_clock_ms" to exit.timestamp.toString(), "association_status" to relation,
                            "previous_application_title_id" to association?.title.orEmpty(), "previous_application_identity_source" to (association?.titleSource ?: "unknown"), "previous_diagnostics_enabled" to (association?.enabled?.toString() ?: "unknown"),
                            "previous_launch_start_observed" to (association?.startObserved?.toString() ?: "unknown")) +
                            ExitRecordIdentity.observationFields(seen, id, exit.timestamp, exit.reason, exit.status, exit.pid, scope) + previous()
                    }
                    if (accepted && flush()) ExitRecordIdentity.markObserved(seen, id, exit.status)
                }
                if (id == selected?.third && !traceAttempted && NativeTraceImport.eligible(seen, id)) {
                    traceAttempted = true
                    NativeTraceImport.attempt(seen, id, {
                        if (apiLevel < 31) NativeTraceSummary.Result("android_api_below_31")
                        else try {
                            queryBounded(traceTimeoutMilliseconds) { NativeTraceSummary.read(traceInput(exit), exit.pid, ownUid) }
                        } catch (_: java.util.concurrent.TimeoutException) { NativeTraceSummary.Result("observer_deadline") }
                        catch (_: Exception) { NativeTraceSummary.Result("read_unavailable") }
                    }) { trace, attempt ->
                        val accepted = event("PREVIOUS_NATIVE_TRACE") {
                            NativeTraceImport.fields(mapOf("source" to "Android_ApplicationExitInfo", "historical_record" to "true", "exit_record_id" to id,
                                "reason_code" to exit.reason.toString(), "status" to exit.status.toString(),
                                "exit_wall_clock_ms" to exit.timestamp.toString(), "trace_scope" to "host_process_candidate_not_guest",
                                "association_status" to relation, "previous_application_title_id" to association?.title.orEmpty(),
                                "previous_application_identity_source" to (association?.titleSource ?: "unknown")) + previous(), trace, attempt)
                        }
                        accepted && flush()
                    }
                }
            }
        }
    }

}
