// SPDX-License-Identifier: MPL-2.0
package emu.skyline.diagnostics

import android.app.Application
import android.content.Context
import emu.skyline.BuildConfig
import java.io.File
import java.util.UUID
import java.util.Locale
import java.lang.ref.WeakReference

object Diagnostics {
    @Volatile private var active = false
    private var signature = ""
    @Volatile private var sessionId = ""
    private var exitHistoryQueried = false
    private val launch = LaunchIdentity()
    private var applicationTitleId = ""
    private var applicationIdentitySource = "unknown"
    private var launchApplicationHint = ""
    private var appContext: Context? = null
    private val processRunId: String by lazy { UUID.randomUUID().toString() }
    private class LaunchFailureBoundary(val owner: Long, var observed: ProcessAssociation?, val hint: String)
    private data class PendingLaunchFailure(val error: WeakReference<Throwable>, val id: String, val owner: Long,
        val observed: ProcessAssociation?, val hint: String) {
        fun fields() = linkedMapOf("failure_id" to id, "failure_generation" to owner.toString(),
            "failure_context_status" to if (observed == null) "not_observed" else "observed_owned_launch",
            "failure_process_run_id" to observed?.process.orEmpty(), "failure_session_id" to observed?.session.orEmpty(),
            "failure_launch_id" to observed?.launch.orEmpty(), "failure_application_title_id" to observed?.title.orEmpty(),
            "failure_application_identity_source" to (observed?.titleSource ?: "unknown"), "failure_commit" to observed?.commit.orEmpty(),
            "failure_ui_application_title_id" to hint,
            "failure_ui_identity_source" to if (hint.isEmpty()) "unknown" else "ui_metadata_hint_not_runtime_verified")
    }
    // One small scope on the launch thread; one weak throwable association until
    // that same thread's uncaught handler consumes it. No global identity restore.
    private val launchFailureBoundary = ThreadLocal<LaunchFailureBoundary?>()
    private val pendingLaunchFailure = ThreadLocal<PendingLaunchFailure?>()
    val processScope: String get() = if (Application.getProcessName().endsWith(":emulationProcess")) "emulation" else "main"
    fun preferences(context: Context) = context.getSharedPreferences("diagnostics", Context.MODE_PRIVATE)
    fun rootDirectory(context: Context) = File(context.filesDir.canonicalFile, "diagnostics")
    fun directory(context: Context) = File(rootDirectory(context), processScope)
    fun exportDirectory(context: Context) = File(context.cacheDir.canonicalFile, "diagnostics-exports")
    @JvmStatic external fun configureNative(path: String, enabled: Boolean, verbose: Boolean, maxFiles: Int, session: String, categories: String, processRunId: String): Boolean
    @JvmStatic external fun setLaunchNative(launchId: String, applicationTitleId: String, newLaunch: Boolean)
    @JvmStatic external fun statusNative(): LongArray
    @JvmStatic external fun flushNative(): Boolean
    @JvmStatic external fun stopNative()
    @JvmStatic external fun emitNative(category: String, event: String, detail: String, titleId: String)
    @JvmStatic external fun emitFieldsNative(category: String, event: String, level: String, keys: Array<String>, values: Array<String>): Boolean
    fun fieldsEvent(category: String, event: String, level: String = "INFO", fields: () -> Map<String, String>): Boolean {
        if (!active) return false
        return runCatching {
            val bounded = fields().entries.take(24).associate { it.key.take(64) to DiagnosticExport.redact(it.value.take(2048)) }
            emitFieldsNative(category, event, level, bounded.keys.toTypedArray(), bounded.values.toTypedArray())
        }.getOrDefault(false)
    }
    @Synchronized internal fun fieldsForSession(expected: String, event: String, level: String = "INFO", fields: () -> Map<String, String>): Boolean =
        active && sessionId == expected && fieldsEvent("lifecycle", event, level, fields)
    @Synchronized fun apply(context: Context) {
        appContext = context.applicationContext
        val prefs = preferences(context)
        val enabled = prefs.getBoolean("enabled", false)
        val categories = prefs.getString("categories", "") ?: ""
        val nextSignature = "$categories:$enabled:${prefs.getBoolean("verbose", false)}:${prefs.getInt("files", 4)}"
        if (signature == nextSignature && (!enabled || status().enabled)) return
        active = false
        val launchObservedLate = enabled && launch.active && launch.id.isEmpty()
        if (enabled) {
            val launchId = launch.observe(true)
            runCatching { setLaunchNative(launchId, "", false) }
            sessionId = UUID.randomUUID().toString()
            // Each process owns one directory. Never share a rotating writer across processes.
            runCatching {
                val directory = directory(context)
                check(directory.isDirectory || directory.mkdirs())
                val temporary = File(directory, "session-id.tmp")
                temporary.writeText(sessionId, Charsets.US_ASCII)
                check(temporary.renameTo(File(directory, "session-id.txt")))
            }
        }
        active = runCatching {
            configureNative(if (enabled) directory(context).absolutePath else "", enabled, prefs.getBoolean("verbose", false), prefs.getInt("files", 4), sessionId, categories, if (enabled) processRunId else "")
        }.getOrDefault(false)
        signature = if (enabled && !active) "" else nextSignature
        if (active) fieldsEvent("lifecycle", "SESSION_METADATA") {
            mapOf("detail" to "process=$processScope process_run_id=$processRunId session=$sessionId application_id=${BuildConfig.APPLICATION_ID} version=${BuildConfig.VERSION_NAME} build=${BuildConfig.BUILD_TYPE} commit=${BuildConfig.DIAGNOSTICS_COMMIT} launch_start_observed=${launch.startObserved}",
                "process_scope" to processScope, "session_id" to sessionId, "process_run_id" to processRunId,
                "application_id" to BuildConfig.APPLICATION_ID, "version" to BuildConfig.VERSION_NAME,
                "build_type" to BuildConfig.BUILD_TYPE, "commit" to BuildConfig.DIAGNOSTICS_COMMIT,
                "launch_id" to launch.id, "launch_start_observed" to launch.startObserved.toString())
        }
        if (active) publishAssociation(context) else {
            runCatching { ProcessStateAssociation.clear() }
            emu.skyline.loader.DiagnosticIdentityStore.clear()
        }
        if (active && launchObservedLate) event("lifecycle", "LAUNCH_CONTEXT_OBSERVED") { "start_observed=false" }
        if (active && !exitHistoryQueried) {
            exitHistoryQueried = true
            runCatching { PreviousProcessExit.schedule(context.applicationContext, sessionId) }
                .onFailure { event("lifecycle", "PROCESS_EXIT_HISTORY_UNAVAILABLE") { "reason=observer_thread_start_failed" } }
        }
    }
    internal fun normalizedTitleId(titleId: String?): String = titleId?.uppercase(Locale.ROOT)
        ?.takeIf { it.length == 16 && it != "0000000000000000" && it.all { char -> char in '0'..'9' || char in 'A'..'F' } }.orEmpty()
    fun event(category: String, event: String, titleId: String? = null, detail: () -> String) {
        if (active) runCatching { emitNative(category, event, DiagnosticExport.redact(detail().take(4096)), normalizedTitleId(titleId)) }
    }
    @Synchronized fun beginLaunch(applicationTitleId: String?, owner: Long = 0) {
        val id = launch.begin(active, owner)
        // A UI hint may be cached. History stays unknown until this generation
        // reports the identity resolved by the native runtime loader.
        this.applicationTitleId = ""
        applicationIdentitySource = "unknown"
        launchApplicationHint = normalizedTitleId(applicationTitleId)
        runCatching { setLaunchNative(id, normalizedTitleId(applicationTitleId), true) }
        event("lifecycle", "LAUNCH_BEGIN", applicationTitleId) { "start_observed=true" }
        if (active) appContext?.let(::publishAssociation)
    }
    /** Called on the ordinary JNI launch thread, after the sink releases its mutex. */
    @JvmStatic fun onNativeContentIdentity(owner: Long, titleId: String, source: String) {
        synchronized(this) { // Same instance monitor as begin/end and failure capture.
            if (!launch.active || !launch.belongsTo(owner)) return
            val id = normalizedTitleId(titleId)
            val knownSource = source in setOf("cnmt_application", "cnmt_patch_parent", "cnmt_aoc_parent", "program_header")
            applicationTitleId = if (knownSource) id else ""
            applicationIdentitySource = if (knownSource && id.isNotEmpty()) source else "unknown"
            if (active) {
                launchFailureBoundary.get()?.takeIf { it.owner == owner }?.observed = currentAssociation()
                appContext?.let(::publishAssociation)
            }
        }
    }
    @Synchronized internal fun currentAssociation() = ProcessAssociation(processRunId, launch.id, sessionId,
        BuildConfig.DIAGNOSTICS_COMMIT, applicationTitleId, processScope, active, launch.startObserved, applicationIdentitySource)
    private fun publishAssociation(context: Context) {
        runCatching { ProcessStateAssociation.offer(context, currentAssociation()) }
    }
    internal fun flushForSession(expected: String): Boolean = active && sessionId == expected &&
        runCatching { flushNative() }.getOrDefault(false) && active && sessionId == expected
    @Synchronized internal fun eventForSession(expected: String, category: String, event: String, detail: () -> String) {
        if (active && sessionId == expected) event(category, event, detail = detail)
    }
    private fun ownedFailureAssociation(owner: Long): ProcessAssociation? =
        if (active && launch.active && launch.belongsTo(owner)) currentAssociation() else null
    /** Observe only; the caller still owns finally cleanup and the original Throwable. */
    internal fun observeLaunchFailure(owner: Long, execute: () -> Unit) {
        val previous = runCatching { launchFailureBoundary.get() }.getOrNull()
        val boundary = runCatching { synchronized(this) { LaunchFailureBoundary(owner, ownedFailureAssociation(owner),
            if (launch.active && launch.belongsTo(owner)) launchApplicationHint else "") } }.getOrNull()
        runCatching { pendingLaunchFailure.remove(); launchFailureBoundary.set(boundary) }
        try { execute() }
        catch (error: Throwable) {
            runCatching { recordLaunchFailure(owner, boundary, error) }
            throw error
        } finally {
            runCatching { if (previous == null) launchFailureBoundary.remove() else launchFailureBoundary.set(previous) }
        }
    }
    @Synchronized private fun recordLaunchFailure(owner: Long, boundary: LaunchFailureBoundary?, error: Throwable) {
        if (pendingLaunchFailure.get()?.error?.get() === error) return // Preserve the innermost observer of this same failure.
        // If B has started, retain A's last owned observation without reading B
        // into A's failure or writing A back into native/platform state.
        val observed = ownedFailureAssociation(owner) ?: boundary?.observed
        val hint = if (launch.active && launch.belongsTo(owner)) launchApplicationHint else boundary?.hint.orEmpty()
        val failure = PendingLaunchFailure(WeakReference(error), if (active) UUID.randomUUID().toString() else "", owner, observed, hint)
        pendingLaunchFailure.set(failure)
        if (active) {
            fieldsEvent("exception", "LAUNCH_FAILURE_CONTEXT") {
                failure.fields() + mapOf("exception_type" to error.javaClass.name, "phase" to "before_launch_cleanup")
            }
            runCatching { flushNative() } // Existing native Flush has a 250ms deadline.
        }
    }
    /** Native completion owns state cleanup even when Activity navigation was rejected. */
    @Synchronized fun endLaunch(owner: Long): Boolean {
        if (!launch.end(owner)) return false
        applicationTitleId = ""
        applicationIdentitySource = "unknown"
        launchApplicationHint = ""
        runCatching { setLaunchNative("", "", true) }
        runCatching { ProcessStateAssociation.clear() }
        return true
    }
    @Synchronized fun finishSession(owner: Long? = null) {
        if (owner != null && !launch.belongsTo(owner)) return
        runCatching { stopNative() }
        active = false
        signature = ""
        launch.end()
        applicationTitleId = ""
        applicationIdentitySource = "unknown"
        launchApplicationHint = ""
        runCatching { ProcessStateAssociation.clear() }
    }
    fun installExceptionObserver() {
        val previous = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler { thread, error ->
            try {
                val failure = pendingLaunchFailure.get()?.takeIf { it.error.get() === error }
                pendingLaunchFailure.remove()
                fieldsEvent("exception", "ANDROID_UNCAUGHT_EXCEPTION", "ERROR") {
                    failure?.fields().orEmpty() + mapOf("thread" to thread.name, "exception_type" to error.javaClass.name,
                        "message" to error.message.orEmpty(), "stack" to error.stackTrace.take(16).joinToString(" | "))
                }
                if (active) runCatching { flushNative() }
            } finally {
                previous?.uncaughtException(thread, error)
            }
        }
    }
    /** File markers avoid stale SharedPreferences caches when the other Android process restarts. */
    fun sessions(context: Context): Map<String, String> = listOf("main", "emulation").associateWith { scope ->
        runCatching {
            val file = File(rootDirectory(context), "$scope/session-id.txt")
            require(file.absoluteFile == file.canonicalFile && file.length() <= 128)
            file.readText(Charsets.US_ASCII).trim().also { require(Regex("[0-9a-f-]{36}").matches(it)) }
        }.getOrDefault("—")
    }
    fun session(context: Context) = sessionId.ifEmpty { sessions(context)[processScope] ?: "—" }
    data class Status(val available: Boolean, val enabled: Boolean = false, val written: Long = 0, val dropped: Long = 0, val diskErrors: Long = 0)
    fun status(): Status = runCatching {
        val values = statusNative()
        require(values.size >= 4)
        Status(true, values[0] != 0L, values[1], values[2], values[3])
    }.getOrDefault(Status(false))
    fun isActive() = active
}
