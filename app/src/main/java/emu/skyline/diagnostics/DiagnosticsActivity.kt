// SPDX-License-Identifier: MPL-2.0
package emu.skyline.diagnostics

import android.os.Build
import android.os.Bundle
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.widget.SwitchCompat
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.ViewModel
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import emu.skyline.BuildConfig
import emu.skyline.getPublicFilesDir
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

internal class DiagnosticsExportState : ViewModel() {
    var snapshot: DiagnosticExport.Snapshot? = null
    var writing = false
    var preparing = false
}

class DiagnosticsActivity : AppCompatActivity() {
    private lateinit var status: TextView
    private lateinit var exportButton: Button
    private val exportState: DiagnosticsExportState by viewModels()
    private var pendingSnapshot: DiagnosticExport.Snapshot?
        get() = exportState.snapshot
        set(value) { exportState.snapshot = value }
    private val export = registerForActivityResult(ActivityResultContracts.CreateDocument("application/zip")) { uri ->
        val snapshot = pendingSnapshot
        if (uri == null) {
            snapshot?.close(); pendingSnapshot = null
        } else if (snapshot == null) {
            Toast.makeText(this, "Export snapshot expired. Create a new preview.", Toast.LENGTH_LONG).show()
        } else {
            val state = exportState
            state.writing = true
            lifecycleScope.launch {
                val resolver = applicationContext.contentResolver
                try {
                    val result = withContext(Dispatchers.IO + NonCancellable) {
                        runCatching {
                            snapshot.use {
                                resolver.openOutputStream(uri, "wt")?.use { output ->
                                    DiagnosticExport.write(output, snapshot)
                                } ?: error("Cannot open destination")
                            }
                        }
                    }
                    Toast.makeText(this@DiagnosticsActivity, if (result.isSuccess) "Diagnostics exported" else "Export failed: ${result.exceptionOrNull()?.javaClass?.simpleName}", Toast.LENGTH_LONG).show()
                } finally {
                    state.snapshot = null; state.writing = false
                }
            }
        }
    }

    private fun textLog() = File(getPublicFilesDir().canonicalFile, "logs/emulation.log")

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        if (pendingSnapshot == null && !exportState.writing) {
            pendingSnapshot = savedInstanceState?.getString("diagnosticSnapshot")?.let {
                runCatching { DiagnosticExport.restore(Diagnostics.exportDirectory(this), it) }.getOrNull()
            }
        }
        title = "Developer diagnostics / 开发者诊断"
        val prefs = Diagnostics.preferences(this)
        val layout = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(32, 32, 32, 32) }
        setContentView(ScrollView(this).apply { addView(layout) })
        fun toggle(label: String, key: String) {
            layout.addView(SwitchCompat(this).apply {
                text = label; isChecked = prefs.getBoolean(key, false)
                setOnCheckedChangeListener { _, checked ->
                    prefs.edit().putBoolean(key, checked).commit()
                    applySettings()
                }
            })
        }
        toggle("Enable diagnostics / 启用诊断 (default off)", "enabled")
        toggle("Detailed tracing / 详细追踪", "verbose")
        layout.addView(Button(this).apply {
            text = "Retention / 保留: 2, 4 or 8 × 2 MiB + up to 128 KiB state per process"
            setOnClickListener {
                AlertDialog.Builder(this@DiagnosticsActivity).setTitle("Maximum log space per process")
                    .setItems(arrayOf("2 files / 4 MiB", "4 files / 8 MiB", "8 files / 16 MiB")) { _, index ->
                        prefs.edit().putInt("files", intArrayOf(2, 4, 8)[index]).commit()
                        applySettings()
                    }.show()
            }
        })
        layout.addView(Button(this).apply {
            text = "Diagnostic categories / 诊断项目"
            setOnClickListener {
                val names = arrayOf("content", "storage", "guest", "exception")
                val saved = prefs.getString("categories", "") ?: ""
                val selected = BooleanArray(names.size) { saved.isEmpty() || names[it] in saved.split(",") }
                AlertDialog.Builder(this@DiagnosticsActivity).setTitle("Lifecycle events always enabled")
                    .setMultiChoiceItems(names, selected) { _, index, checked -> selected[index] = checked }
                    .setPositiveButton(android.R.string.ok) { _, _ ->
                        val categories = names.filterIndexed { index, _ -> selected[index] }.joinToString(",").ifEmpty { "none" }
                        prefs.edit().putString("categories", categories).commit()
                        applySettings()
                    }.setNegativeButton(android.R.string.cancel, null).show()
            }
        })
        status = TextView(this); layout.addView(status)
        layout.addView(TextView(this).apply {
            text = "Settings apply here immediately and to a separate emulation process when it next starts. Restart emulation after changing settings in release builds.\n\nv1 captures content, storage, guest logs and exceptions. DFX events require real guest messages. Internal Lua stacks are unavailable unless supplied by the game."
        })
        exportButton = Button(this).apply {
            text = "Preview and export / 预览并导出"
            setOnClickListener { prepareExport() }
        }
        layout.addView(exportButton)
        lifecycleScope.launch {
            repeatOnLifecycle(Lifecycle.State.STARTED) {
                while (true) {
                    refresh()
                    delay(1000)
                }
            }
        }
    }

    override fun onSaveInstanceState(outState: Bundle) {
        pendingSnapshot?.let { outState.putString("diagnosticSnapshot", it.id) }
        super.onSaveInstanceState(outState)
    }

    private fun applySettings() = lifecycleScope.launch {
        withContext(Dispatchers.IO) { Diagnostics.apply(this@DiagnosticsActivity) }
        refresh()
    }

    private fun prepareExport() = lifecycleScope.launch {
        if (exportState.writing || exportState.preparing) return@launch
        exportState.preparing = true
        exportButton.isEnabled = false
        val result = try { withContext(Dispatchers.IO) {
            runCatching {
                pendingSnapshot?.close()
                val state = Diagnostics.status()
                val prefs = Diagnostics.preferences(this@DiagnosticsActivity)
                val metadata = buildString {
                    appendLine("schema=1")
                    appendLine("exporter_version=${BuildConfig.VERSION_NAME}")
                    appendLine("exporter_application_id=${BuildConfig.APPLICATION_ID}")
                    appendLine("exporter_build=${BuildConfig.BUILD_TYPE}")
                    appendLine("exporter_flavor=${BuildConfig.FLAVOR}")
                    appendLine("exporter_commit=${BuildConfig.DIAGNOSTICS_COMMIT}")
                    appendLine("android_api=${Build.VERSION.SDK_INT}")
                    appendLine("device_model=${Build.MANUFACTURER} ${Build.MODEL}")
                    appendLine("status_process=${Diagnostics.processScope}")
                    appendLine("status_session_id=${Diagnostics.session(this@DiagnosticsActivity)}")
                    appendLine("status_counters_scope=current process and current sink session only; exported logs may contain older sessions")
                    appendLine("status_available=${state.available}")
                    appendLine("sink_active=${state.enabled}")
                    appendLine("written=${state.written}; dropped=${state.dropped}; disk_errors=${state.diskErrors}")
                    appendLine("configured_enabled=${prefs.getBoolean("enabled", false)}")
                    appendLine("configured_detailed=${prefs.getBoolean("verbose", false)}")
                    appendLine("configured_categories=${prefs.getString("categories", "").orEmpty().ifEmpty { "all" }}")
                    appendLine("configured_retention_per_process=${prefs.getInt("files", 4)} x 2 MiB")
                    appendLine("configured_retention_two_processes=2 x ${prefs.getInt("files", 4)} x 2 MiB")
                    Diagnostics.sessions(this@DiagnosticsActivity).forEach { (scope, session) -> appendLine("last_${scope}_session=$session") }
                    appendLine("Separate process session IDs are last-observed identifiers, not proof that the process is still running.")
                    appendLine("No game content or key files included. Content summaries describe observed associations, not playability.")
                }
                DiagnosticExport.capture(Diagnostics.rootDirectory(this@DiagnosticsActivity), textLog(), Diagnostics.exportDirectory(this@DiagnosticsActivity), metadata)
            }
        } } finally { exportState.preparing = false }
        exportButton.isEnabled = true
        val snapshot = result.getOrElse {
            Toast.makeText(this@DiagnosticsActivity, "Preview failed: ${it.javaClass.simpleName}", Toast.LENGTH_LONG).show()
            return@launch
        }
        pendingSnapshot = snapshot
        var exporting = false
        AlertDialog.Builder(this@DiagnosticsActivity).setTitle("Export snapshot / 导出快照")
            .setMessage(snapshot.entries.joinToString("\n") { "${it.name} (${it.bytes} bytes)" } +
                "\n\nThis prepared snapshot is fixed, with at most 40 MiB of logs plus metadata. Omitted records and files are counted in metadata. URIs, private paths and recognized credential lines are redacted. Guest messages may still contain personal text; review before sharing.")
            .setPositiveButton("Export") { _, _ ->
                exporting = true
                export.launch("pine-diag-${Diagnostics.session(this@DiagnosticsActivity)}.zip")
            }
            .setNegativeButton(android.R.string.cancel, null)
            .setOnDismissListener {
                if (!exporting) { snapshot.close(); pendingSnapshot = null }
            }.show()
    }

    private suspend fun refresh() {
        status.text = withContext(Dispatchers.IO) {
            runCatching {
                val state = Diagnostics.status()
                val root = Diagnostics.rootDirectory(this@DiagnosticsActivity)
                val files = DiagnosticExport.files(root, textLog())
                val sessions = Diagnostics.sessions(this@DiagnosticsActivity)
                buildString {
                    appendLine("Status process: ${Diagnostics.processScope}")
                    appendLine("Session: ${Diagnostics.session(this@DiagnosticsActivity)}")
                    appendLine("Sink active: ${if (state.available) state.enabled else "unavailable"}")
                    appendLine("Written: ${state.written}, dropped: ${state.dropped}, disk errors: ${state.diskErrors}")
                    listOf("main", "emulation").forEach { scope ->
                        val logs = files.filter { it.parentFile == File(root, scope) }
                        appendLine("$scope: last session ${sessions[scope]}, ${logs.size} logs, ${logs.sumOf { it.length() }} bytes")
                    }
                    val retention = Diagnostics.preferences(this@DiagnosticsActivity).getInt("files", 4)
                    append("Limit per process: $retention × 2 MiB + up to 128 KiB state; two scopes: up to ${retention * 4} MiB + 256 KiB state")
                }
            }.getOrDefault("Diagnostics status unavailable")
        }
        exportButton.isEnabled = !exportState.writing && !exportState.preparing
    }
}
