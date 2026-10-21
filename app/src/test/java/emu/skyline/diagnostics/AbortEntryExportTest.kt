// SPDX-License-Identifier: MPL-2.0
package emu.skyline.diagnostics

import kotlinx.serialization.json.*
import org.junit.Assert.*
import org.junit.Test
import java.io.ByteArrayOutputStream
import java.io.File
import java.nio.file.Files
import java.security.MessageDigest
import java.util.zip.ZipInputStream

class AbortEntryExportTest {
    /** Regular JVM CI uses an explicitly offline row. Dedicated ARM64 CI supplies
     * actual writer files from its executed fixture; neither input is a game. */
    @Test fun entryRecordExportRetainsPreciseStringsAndObservationScope() {
        val root = Files.createTempDirectory("abort-entry-export").toFile()
        try {
            val logs = File(root, "logs").apply { mkdir() }
            val main = File(logs, "main").apply { mkdir() }
            val cache = File(root, "cache").apply { mkdir() }
            val text = File(root, "emulation.log").apply { writeText("") }
            val input = System.getenv("PINE_ABORT_ENTRY_CAPTURE")?.let(::File)
            val mode = if (input == null) "offline_export_contract_only" else "executed_aarch64_fixture_not_game"
            if (input == null) {
                val row = buildJsonObject {
                    put("schema", 1); put("session_id", "offline-session"); put("sequence", 1)
                    put("event", "SYNTHETIC_ABORT_ENTRY"); put("level", "INFO")
                    putJsonObject("fields") {
                        put("entry_x0", "0xfedcba9876543210"); put("original_caller_lr", "0x00007100002d64e4")
                        put("observation_scope", mode)
                    }
                }.toString() + "\n"
                File(main, "diagnostics-0.jsonl").writeText(row)
                File(main, "resource-window.jsonl").writeText(row)
            } else {
                for (name in listOf("diagnostics-0.jsonl", "resource-window.jsonl")) {
                    val source = File(input, name)
                    assertTrue("Missing executed fixture file $name", source.isFile)
                    assertTrue("Unbounded fixture input", source.length() in 1L..(128 * 1024).toLong())
                    source.copyTo(File(main, name))
                }
            }
            val original = File(main, "diagnostics-0.jsonl").readLines().mapNotNull { line ->
                if (line.isBlank()) null else Json.parseToJsonElement(line).jsonObject
            }.single { it["event"]?.jsonPrimitive?.content == "SYNTHETIC_ABORT_ENTRY" }
            assertEquals(mode, original.getValue("fields").jsonObject.getValue("observation_scope").jsonPrimitive.content)
            val archive = ByteArrayOutputStream()
            DiagnosticExport.capture(logs, text, cache, "synthetic_entry_input=$mode").use { snapshot ->
                DiagnosticExport.write(archive, snapshot)
            }
            val entries = mutableMapOf<String, String>()
            ZipInputStream(archive.toByteArray().inputStream()).use { zip ->
                while (true) {
                    val entry = zip.nextEntry ?: break
                    entries[entry.name] = zip.readBytes().toString(Charsets.UTF_8)
                }
            }
            // The existing exporter visits the resource window first and
            // deduplicates identical (session,kind,sequence) across files.
            // Preserve that policy; an entry must survive exactly once.
            assertTrue(entries.containsKey("main/diagnostics-0.jsonl"))
            val rows = entries.filterKeys { it.endsWith(".jsonl") }.values.flatMap { text ->
                text.lineSequence().filter { it.isNotBlank() }.map { Json.parseToJsonElement(it).jsonObject }.toList()
            }
            val exported = rows.single { it["event"]?.jsonPrimitive?.content == "SYNTHETIC_ABORT_ENTRY" }
            assertEquals(original, exported)
            assertEquals("0xfedcba9876543210", exported.getValue("fields").jsonObject.getValue("entry_x0").jsonPrimitive.content)
            assertTrue(entries.getValue("main/resource-window.jsonl").lineSequence().filter { it.isNotBlank() }
                .map { Json.parseToJsonElement(it).jsonObject }.any { it == exported })
            assertTrue(entries.getValue("session-metadata.txt").contains("observed_error_count=0"))
            val bytes = archive.toByteArray()
            val sha = MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it) }
            val report = buildJsonObject {
                put("scope", mode); put("android_app_run", false); put("game_observation", false)
                put("actual_DiagnosticExport_zip", true); put("zip_sha256", sha)
                put("entry_x0", "0xfedcba9876543210"); put("entry_rows_across_archive", 1)
                put("resource_window_entry_preserved", true); put("existing_export_dedup_preserved", true)
            }
            System.getenv("PINE_ABORT_ENTRY_EXPORT_REPORT")?.let { path ->
                File(path).apply { parentFile!!.mkdirs(); writeText(report.toString() + "\n") }
                File(path).resolveSibling("entry-export.zip").writeBytes(bytes)
            }
            println(report)
        } finally { root.deleteRecursively() }
    }
}
