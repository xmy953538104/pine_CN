// SPDX-License-Identifier: MPL-2.0
package emu.skyline.diagnostics

import android.os.Bundle
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.widget.SwitchCompat
import androidx.activity.result.contract.ActivityResultContracts
import androidx.lifecycle.lifecycleScope
import emu.skyline.BuildConfig
import emu.skyline.getPublicFilesDir
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

class DiagnosticsActivity : AppCompatActivity() {
    private lateinit var status: TextView
    private val export = registerForActivityResult(ActivityResultContracts.CreateDocument("application/zip")) { uri ->
        if (uri != null) lifecycleScope.launch {
            val result = withContext(Dispatchers.IO) {
                runCatching {
                    val files = DiagnosticExport.files(Diagnostics.directory(this@DiagnosticsActivity), File(getPublicFilesDir(), "logs/emulation.log"))
                    contentResolver.openOutputStream(uri)?.use {
                        DiagnosticExport.write(it, files, "schema=1\nsession=${Diagnostics.session(this@DiagnosticsActivity)}\nversion=${BuildConfig.VERSION_NAME}\nbuild=${BuildConfig.BUILD_TYPE}\nEvents contain observed title/update/DLC association and errors. No game content included.\n")
                    } ?: error("Cannot open destination")
                }
            }
            Toast.makeText(this@DiagnosticsActivity, if (result.isSuccess) "Diagnostics exported" else "Export failed: ${result.exceptionOrNull()?.javaClass?.simpleName}", Toast.LENGTH_LONG).show()
        }
    }
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        title = "Developer diagnostics / 开发者诊断"
        val prefs = Diagnostics.preferences(this)
        val layout = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(32, 32, 32, 32) }
        setContentView(ScrollView(this).apply { addView(layout) })
        fun toggle(label: String, key: String) {
            layout.addView(SwitchCompat(this).apply {
                text = label; isChecked = prefs.getBoolean(key, false)
                setOnCheckedChangeListener { _, checked ->
                    prefs.edit().putBoolean(key, checked).commit()
                    Diagnostics.apply(this@DiagnosticsActivity); refresh()
                }
            })
        }
        toggle("Enable diagnostics / 启用诊断 (default off)", "enabled")
        toggle("Detailed tracing / 详细追踪", "verbose")
        layout.addView(Button(this).apply {
            text = "Retention / 保留: 2, 4 or 8 × 2 MiB"
            setOnClickListener {
                AlertDialog.Builder(this@DiagnosticsActivity).setTitle("Maximum log space")
                    .setItems(arrayOf("2 files / 4 MiB", "4 files / 8 MiB", "8 files / 16 MiB")) { _, index ->
                        prefs.edit().putInt("files", intArrayOf(2, 4, 8)[index]).commit()
                        Diagnostics.apply(this@DiagnosticsActivity); refresh()
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
                        Diagnostics.apply(this@DiagnosticsActivity); refresh()
                    }.setNegativeButton(android.R.string.cancel, null).show()
            }
        })
        status = TextView(this); layout.addView(status)
        layout.addView(TextView(this).apply { text = "v1 captures content, storage, guest logs and exceptions. DFX events require real guest messages. Internal Lua stacks are unavailable unless supplied by the game." })
        layout.addView(Button(this).apply {
            text = "Preview and export / 预览并导出"
            setOnClickListener {
                val files = DiagnosticExport.files(Diagnostics.directory(this@DiagnosticsActivity), File(getPublicFilesDir(), "logs/emulation.log"))
                AlertDialog.Builder(this@DiagnosticsActivity).setTitle("Export allowlist / 导出清单")
                    .setMessage("session-metadata.txt\n" + files.joinToString("\n") { "${it.name} (${it.length()} bytes)" } + "\n\nURIs, private paths and credential lines are redacted. Guest messages may still contain personal text; review before sharing.")
                    .setPositiveButton("Export") { _, _ -> export.launch("pine-diag-${Diagnostics.session(this@DiagnosticsActivity)}.zip") }
                    .setNegativeButton(android.R.string.cancel, null).show()
            }
        })
        refresh()
    }
    private fun refresh() {
        val files = Diagnostics.directory(this).listFiles()?.filter { Regex("diagnostics-[0-7]\\.jsonl").matches(it.name) } ?: emptyList()
        val stats = Diagnostics.statusNative()
        status.text = "Session: ${Diagnostics.session(this)}\nSink active: ${stats[0] != 0L}\nWritten: ${stats[1]}, dropped: ${stats[2]}, disk errors: ${stats[3]}\nLogs: ${files.size}, ${files.sumOf { it.length() }} bytes\nLimit: ${Diagnostics.preferences(this).getInt("files", 4)} × 2 MiB"
    }
}
