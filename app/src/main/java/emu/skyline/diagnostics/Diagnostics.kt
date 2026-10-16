// SPDX-License-Identifier: MPL-2.0
package emu.skyline.diagnostics

import android.app.Application
import android.content.Context
import emu.skyline.BuildConfig
import java.io.File
import java.util.UUID

object Diagnostics {
    @Volatile private var active = false
    private var signature = ""
    private var sessionId = ""
    val processScope: String get() = if (Application.getProcessName().endsWith(":emulationProcess")) "emulation" else "main"
    fun preferences(context: Context) = context.getSharedPreferences("diagnostics", Context.MODE_PRIVATE)
    fun rootDirectory(context: Context) = File(context.filesDir.canonicalFile, "diagnostics")
    fun directory(context: Context) = File(rootDirectory(context), processScope)
    fun exportDirectory(context: Context) = File(context.cacheDir.canonicalFile, "diagnostics-exports")
    @JvmStatic external fun configureNative(path: String, enabled: Boolean, verbose: Boolean, maxFiles: Int, session: String, categories: String): Boolean
    @JvmStatic external fun statusNative(): LongArray
    @JvmStatic external fun flushNative(): Boolean
    @JvmStatic external fun stopNative()
    @JvmStatic external fun emitNative(category: String, event: String, detail: String)
    @Synchronized fun apply(context: Context) {
        val prefs = preferences(context)
        val enabled = prefs.getBoolean("enabled", false)
        val categories = prefs.getString("categories", "") ?: ""
        val nextSignature = "$categories:$enabled:${prefs.getBoolean("verbose", false)}:${prefs.getInt("files", 4)}"
        if (signature == nextSignature && (!enabled || status().enabled)) return
        active = false
        if (enabled) {
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
            configureNative(if (enabled) directory(context).absolutePath else "", enabled, prefs.getBoolean("verbose", false), prefs.getInt("files", 4), sessionId, categories)
        }.getOrDefault(false)
        signature = if (enabled && !active) "" else nextSignature
        if (active) event("lifecycle", "SESSION_METADATA") {
            "process=$processScope session=$sessionId version=${BuildConfig.VERSION_NAME} build=${BuildConfig.BUILD_TYPE} commit=${BuildConfig.DIAGNOSTICS_COMMIT}"
        }
    }
    fun event(category: String, event: String, detail: () -> String) {
        if (active) runCatching { emitNative(category, event, DiagnosticExport.redact(detail().take(4096))) }
    }
    @Synchronized fun finishSession() {
        runCatching { stopNative() }
        active = false
        signature = ""
    }
    fun installExceptionObserver() {
        val previous = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler { thread, error ->
            try {
                event("exception", "ANDROID_UNCAUGHT_EXCEPTION") { "thread=${thread.name} type=${error.javaClass.name} message=${error.message} stack=${error.stackTrace.take(16).joinToString(" | ")}" }
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
