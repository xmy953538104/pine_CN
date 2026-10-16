// SPDX-License-Identifier: MPL-2.0
package emu.skyline.diagnostics

import android.content.Context
import java.io.File
import java.util.UUID

object Diagnostics {
    @Volatile private var active = false
    private var signature = ""
    private var sessionId = ""
    fun preferences(context: Context) = context.getSharedPreferences("diagnostics", Context.MODE_PRIVATE)
    fun directory(context: Context) = File(context.filesDir, "diagnostics")
    @JvmStatic external fun configureNative(path: String, enabled: Boolean, verbose: Boolean, maxFiles: Int, session: String, categories: String): Boolean
    @JvmStatic external fun statusNative(): LongArray
    @JvmStatic external fun emitNative(category: String, event: String, detail: String)
    @Synchronized fun apply(context: Context) {
        val prefs = preferences(context)
        val enabled = prefs.getBoolean("enabled", false)
        val categories = prefs.getString("categories", "") ?: ""
        val nextSignature = "$categories:$enabled:${prefs.getBoolean("verbose", false)}:${prefs.getInt("files", 4)}"
        if (signature == nextSignature) return
        if (enabled) {
            sessionId = UUID.randomUUID().toString()
            prefs.edit().putString("session", sessionId).apply()
        }
        active = configureNative(directory(context).absolutePath, enabled, prefs.getBoolean("verbose", false), prefs.getInt("files", 4), sessionId, categories)
        signature = if (enabled && !active) "" else nextSignature
    }
    fun event(category: String, event: String, detail: () -> String) {
        if (active) runCatching { emitNative(category, event, DiagnosticExport.redact(detail().take(4096))) }
    }
    fun installExceptionObserver() {
        val previous = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler { thread, error ->
            try {
                event("exception", "ANDROID_UNCAUGHT_EXCEPTION") { "thread=${thread.name} type=${error.javaClass.name} message=${error.message} stack=${error.stackTrace.take(16).joinToString(" | ")}" }
            } finally {
                previous?.uncaughtException(thread, error)
            }
        }
    }
    fun session(context: Context) = preferences(context).getString("session", "—") ?: "—"
    fun isActive() = active
}
