// SPDX-License-Identifier: MPL-2.0
package emu.skyline.diagnostics

import kotlinx.serialization.json.*
import org.junit.Assert.*
import org.junit.Test
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.RandomAccessFile
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.util.concurrent.CountDownLatch
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference
import java.util.zip.ZipInputStream

class DiagnosticExportTest {
    private fun sandbox(test: (logs: File, textLog: File, cache: File, root: File) -> Unit) {
        val root = Files.createTempDirectory("pine-diag-test").toFile()
        try {
            test(File(root, "diagnostics").apply { mkdir() }, File(root, "emulation.log"), File(root, "cache"), root)
        } finally { root.deleteRecursively() }
    }

    private fun log(logs: File, name: String, text: String) = File(logs, name).apply {
        parentFile!!.mkdirs(); writeText(text, Charsets.UTF_8)
    }

    private fun record(session: String, sequence: Int, category: String = "content", event: String = "BASE_SELECTED", level: String = "INFO", detail: String = "title_id=01000000") = buildJsonObject {
        put("schema", 1); put("session_id", session); put("sequence", sequence)
        put("category", category); put("event", event); put("level", level)
        put("last_stage", "content_ready")
        putJsonObject("fields") { put("detail", detail) }
    }.toString() + "\n"

    private fun archive(snapshot: DiagnosticExport.Snapshot): Map<String, String> {
        val output = ByteArrayOutputStream()
        DiagnosticExport.write(output, snapshot)
        val entries = linkedMapOf<String, String>()
        ZipInputStream(output.toByteArray().inputStream()).use { zip ->
            while (true) {
                val entry = zip.nextEntry ?: break
                assertNull("Duplicate ZIP name", entries.put(entry.name, zip.readBytes().toString(Charsets.UTF_8)))
            }
        }
        assertEquals(snapshot.entries.map { it.name }.toSet(), entries.keys)
        snapshot.entries.forEach { assertEquals(it.bytes, entries.getValue(it.name).toByteArray(Charsets.UTF_8).size.toLong()) }
        return entries
    }

    @Test fun redactionPreservesJsonStructureTypesEscapesAndUtf8() {
        val source = Json.parseToJsonElement("""{"schema":1,"message":"世界 CONTENT:\/\/private\/rom?q=1","password":"quoted \"secret\"","fields":{"token":{"nested":"do-not-leak"},"path":"/data/private/rom","windows":"C:\\Users\\Private\\game.nsp","valid":true,"count":12,"missing":null,"array":["https://private/rom",2,false]},"event":"GUEST_DEBUG_MESSAGE"}""").jsonObject
        val safe = DiagnosticExport.redactJson(source)
        val reparsed = Json.parseToJsonElement(safe.toString()).jsonObject
        assertEquals(source.keys, reparsed.keys)
        assertEquals(1, reparsed.getValue("schema").jsonPrimitive.int)
        assertEquals("GUEST_DEBUG_MESSAGE", reparsed.getValue("event").jsonPrimitive.content)
        assertTrue(reparsed.getValue("message").jsonPrimitive.content.contains("世界"))
        assertEquals(source.getValue("fields").jsonObject.keys, reparsed.getValue("fields").jsonObject.keys)
        val fields = reparsed.getValue("fields").jsonObject
        assertTrue(fields.getValue("valid").jsonPrimitive.boolean)
        assertEquals(12, fields.getValue("count").jsonPrimitive.int)
        assertEquals(JsonNull, fields["missing"])
        assertEquals(2, fields.getValue("array").jsonArray[1].jsonPrimitive.int)
        assertFalse(safe.toString().contains("private", ignoreCase = true))
        assertFalse(safe.toString().contains("do-not-leak"))
        assertFalse(safe.toString().contains("quoted"))
    }

    @Test fun textRedactsCredentialLinesAndKnownLocations() {
        val safe = DiagnosticExport.redact("before 世界\nROM content://authority/game.nsp /storage/emulated/0/private\npassword=DO_NOT_EXPORT rest-of-line\nAuthorization: Bearer DO_NOT_EXPORT\nprod.keys DO_NOT_EXPORT\nafter")
        assertTrue(safe.contains("before 世界")); assertTrue(safe.endsWith("after"))
        assertFalse(safe.contains("authority")); assertFalse(safe.contains("game.nsp"))
        assertFalse(safe.contains("emulated")); assertFalse(safe.contains("DO_NOT_EXPORT"))
        assertFalse(safe.contains("rest-of-line"))
    }

    @Test fun exportUsesOnlyFixedNamesAcrossBothProcessesAndLegacy() = sandbox { logs, textLog, cache, _ ->
        log(logs, "diagnostics-0.jsonl", record("legacy", 1))
        log(logs, "main/diagnostics-0.jsonl", record("main", 1, detail = "世界 content://private/game"))
        log(logs, "emulation/diagnostics-0.jsonl", record("emu", 1))
        log(logs, "prod.keys", "DO_NOT_EXPORT")
        log(logs, "subdirectory/diagnostics-1.jsonl", "DO_NOT_EXPORT")
        log(logs, "main/diagnostics-8.jsonl", "DO_NOT_EXPORT")
        log(logs, "main/session-id.txt", "DO_NOT_EXPORT")
        textLog.writeText("content://private/game.nsp\nhello 世界\n")
        DiagnosticExport.capture(logs, textLog, cache, "schema=1\ncommit=0123456789abcdef").use { snapshot ->
            val entries = archive(snapshot)
            assertEquals(setOf("session-metadata.txt", "diagnostics-0.jsonl", "main/diagnostics-0.jsonl", "emulation/diagnostics-0.jsonl", "emulation.log"), entries.keys)
            assertFalse(entries.values.any { it.contains("private") || it.contains("DO_NOT_EXPORT") })
            assertTrue(entries.getValue("main/diagnostics-0.jsonl").contains("世界"))
            assertTrue(entries.getValue("session-metadata.txt").contains("commit=0123456789abcdef"))
        }
    }

    @Test fun rejectsSymlinkFilesSymlinkParentsAndOversizedLogs() = sandbox { logs, textLog, cache, root ->
        val outside = File(root, "outside").apply { mkdir() }
        val secret = File(outside, "secret").apply { writeText("DO_NOT_EXPORT") }
        Files.createSymbolicLink(File(logs, "diagnostics-0.jsonl").toPath(), secret.toPath())
        File(outside, "diagnostics-0.jsonl").writeText(record("DO_NOT_EXPORT", 1))
        Files.createSymbolicLink(File(logs, "main").toPath(), outside.toPath())
        Files.createSymbolicLink(textLog.toPath(), secret.toPath())
        RandomAccessFile(File(logs, "diagnostics-1.jsonl"), "rw").use { it.setLength(DiagnosticExport.MAX_FILE_BYTES.toLong() + 1) }
        assertTrue(DiagnosticExport.files(logs, textLog).isEmpty())
        DiagnosticExport.capture(logs, textLog, cache, "schema=1").use { snapshot ->
            val entries = archive(snapshot)
            assertEquals(setOf("session-metadata.txt"), entries.keys)
            assertTrue(entries.values.none { it.contains("DO_NOT_EXPORT") })
            assertTrue(entries.getValue("session-metadata.txt").contains("exceeds ${DiagnosticExport.MAX_FILE_BYTES}"))
        }
    }

    @Test fun previewIsStableAfterAppendRenameDeletionAndActivityRestore() = sandbox { logs, textLog, cache, _ ->
        val source = log(logs, "main/diagnostics-0.jsonl", record("before", 1))
        textLog.writeText("before\n")
        DiagnosticExport.capture(logs, textLog, cache, "schema=1").use { snapshot ->
            val preview = snapshot.entries
            source.appendText(record("AFTER", 2))
            Files.move(source.toPath(), File(logs, "main/diagnostics-1.jsonl").toPath())
            source.writeText(record("AFTER", 3))
            textLog.delete()
            val restored = DiagnosticExport.restore(cache, snapshot.id)!!
            assertEquals(preview, restored.entries)
            val entries = archive(restored)
            assertFalse(entries.values.any { it.contains("AFTER") })
            assertEquals("before\n", entries["emulation.log"])
            assertNull(DiagnosticExport.restore(cache, "../${snapshot.id}"))
        }
    }

    @Test fun omitsInvalidPartialAndOversizedRecordsWithoutLeakingRawText() = sandbox { logs, textLog, cache, _ ->
        log(logs, "main/diagnostics-0.jsonl", record("valid", 1) +
            "{broken DO_NOT_EXPORT}\n" + "x".repeat(65537) + "DO_NOT_EXPORT\n" + "{\"message\":\"DO_NOT_EXPORT")
        DiagnosticExport.capture(logs, textLog, cache, "schema=1").use { snapshot ->
            val entries = archive(snapshot)
            val lines = entries.getValue("main/diagnostics-0.jsonl").lineSequence().filter { it.isNotBlank() }.toList()
            assertEquals(1, lines.size)
            assertEquals("valid", Json.parseToJsonElement(lines.single()).jsonObject.getValue("session_id").jsonPrimitive.content)
            val metadata = entries.getValue("session-metadata.txt")
            assertTrue(metadata.contains("omitted_malformed_records=1"))
            assertTrue(metadata.contains("omitted_incomplete_records=1"))
            assertTrue(metadata.contains("omitted_oversized_lines=1"))
            assertFalse(entries.values.any { it.contains("DO_NOT_EXPORT") })
        }
    }

    @Test fun summaryKeepsDistinctSessionsAndDeduplicatesRotatedRecords() = sandbox { logs, textLog, cache, _ ->
        val first = record("main", 1, event = "CONTENT_ASSOCIATION", detail = "title_id=01000000 update_count=2 dlc_count=3")
        log(logs, "main/diagnostics-0.jsonl", first)
        log(logs, "main/diagnostics-1.jsonl", first)
        log(logs, "emulation/diagnostics-0.jsonl", record("emulation", 1, event = "DLC_RUNTIME_ERROR", level = "ERROR", detail = "title_id=01000000 error=short_read /data/private/rom"))
        DiagnosticExport.capture(logs, textLog, cache, "schema=1").use { snapshot ->
            val entries = archive(snapshot)
            val records = entries.filterKeys { it.endsWith(".jsonl") }.values.sumOf { text -> text.lineSequence().count { it.isNotBlank() } }
            assertEquals(2, records)
            val metadata = entries.getValue("session-metadata.txt")
            assertTrue(metadata.contains("omitted_duplicate_records=1"))
            assertTrue(metadata.contains("observed_error_count=1"))
            assertTrue(metadata.contains("CONTENT_ASSOCIATION")); assertTrue(metadata.contains("DLC_RUNTIME_ERROR"))
            assertTrue(metadata.contains("01000000")); assertTrue(metadata.contains("update_count=2 dlc_count=3"))
            assertTrue(metadata.contains("content_ready")); assertFalse(metadata.contains("private"))
        }
    }

    @Test fun metadataDistinguishesCapturedSessionBuildsFromCurrentExporter() = sandbox { logs, textLog, cache, _ ->
        log(logs, "main/diagnostics-0.jsonl", record("old-session", 1, category = "lifecycle", event = "SESSION_METADATA", detail = "version=old commit=11111111 process=main") + record("old-session", 2))
        log(logs, "emulation/diagnostics-0.jsonl", record("new-session", 1, category = "lifecycle", event = "SESSION_METADATA", detail = "version=new commit=22222222 process=emulation") + record("unknown-build-session", 1))
        DiagnosticExport.capture(logs, textLog, cache, "schema=1\nexporter_commit=33333333").use { snapshot ->
            val metadata = archive(snapshot).getValue("session-metadata.txt")
            assertTrue(metadata.contains("exporter_commit=33333333"))
            assertTrue(metadata.contains("version=old commit=11111111 process=main"))
            assertTrue(metadata.contains("version=new commit=22222222 process=emulation"))
            assertTrue(metadata.contains("observed_sessions_without_build_metadata=unknown-build-session"))
        }
    }

    @Test fun abandonedPrivateSnapshotsHaveBoundedRetention() = sandbox { logs, textLog, cache, _ ->
        val snapshots = (0..3).map {
            DiagnosticExport.capture(logs, textLog, cache, "schema=1")
        }
        try {
            assertEquals(2, cache.listFiles()!!.count { it.isDirectory })
            assertNotNull(DiagnosticExport.restore(cache, snapshots.last().id))
        } finally { snapshots.forEach { it.close() } }
    }

    @Test fun prunesInterruptedSnapshotWithoutMetadata() = sandbox { logs, textLog, cache, _ ->
        val incomplete = File(cache, "snapshot-11111111-1111-1111-1111-111111111111")
        log(incomplete, "emulation/diagnostics-0.jsonl", "partial export")
        assertTrue(incomplete.setLastModified(1))
        assertNull(DiagnosticExport.restore(cache, incomplete.name))
        DiagnosticExport.capture(logs, textLog, cache, "schema=1").use {
            assertFalse("Interrupted snapshot was not pruned", incomplete.exists())
        }
    }

    @Test fun snapshotCleanupUnlinksScopeSymlinkWithoutDeletingOutsideLogs() = sandbox { logs, textLog, cache, root ->
        log(logs, "main/diagnostics-0.jsonl", record("main", 1))
        val outside = File(root, "outside").apply { mkdir() }
        val victim = File(outside, "diagnostics-0.jsonl").apply { writeText("DO_NOT_DELETE") }
        val snapshot = DiagnosticExport.capture(logs, textLog, cache, "schema=1")
        val scope = File(cache, "${snapshot.id}/main")
        assertTrue(File(scope, "diagnostics-0.jsonl").delete())
        assertTrue(scope.delete())
        Files.createSymbolicLink(scope.toPath(), outside.toPath())
        snapshot.close()
        assertEquals("DO_NOT_DELETE", victim.readText())
        assertFalse(Files.exists(scope.toPath(), java.nio.file.LinkOption.NOFOLLOW_LINKS))
    }

    @Test fun aggregateBudgetPrioritizesEmulationThenMainOverLegacy() = sandbox { logs, textLog, cache, _ ->
        val emulation = record("emu", 1)
        val main = record("main", 1)
        log(logs, "emulation/diagnostics-0.jsonl", emulation)
        log(logs, "main/diagnostics-0.jsonl", main)
        log(logs, "diagnostics-0.jsonl", record("legacy", 1))
        textLog.writeText("legacy text\n")
        val budget = emulation.toByteArray(Charsets.UTF_8).size + main.toByteArray(Charsets.UTF_8).size
        DiagnosticExport.captureBounded(logs, textLog, cache, "schema=1", budget).use { snapshot ->
            val entries = archive(snapshot)
            assertEquals(emulation, entries["emulation/diagnostics-0.jsonl"])
            assertEquals(main, entries["main/diagnostics-0.jsonl"])
            assertFalse(entries.containsKey("diagnostics-0.jsonl"))
            assertFalse(entries.containsKey("emulation.log"))
            val metadata = entries.getValue("session-metadata.txt")
            assertTrue(metadata.contains("snapshot_exported_log_bytes=$budget"))
            assertTrue(metadata.contains("omitted_file=diagnostics-0.jsonl: snapshot byte limit reached"))
            assertTrue(snapshot.entries.filter { it.name != "session-metadata.txt" }.sumOf { it.bytes } <= budget)
        }
    }

    @Test fun redactionExpansionStillObeysByteLimitAtCompleteJsonBoundaries() = sandbox { logs, textLog, cache, _ ->
        val line = buildJsonObject { put("message", "/data/x ".repeat(6000)) }.toString() + "\n"
        val source = log(logs, "main/diagnostics-0.jsonl", line.repeat(170))
        assertTrue(source.length() <= DiagnosticExport.MAX_FILE_BYTES)
        DiagnosticExport.capture(logs, textLog, cache, "schema=1").use { snapshot ->
            val entries = archive(snapshot)
            val text = entries.getValue("main/diagnostics-0.jsonl")
            assertTrue(text.toByteArray(Charsets.UTF_8).size <= DiagnosticExport.MAX_FILE_BYTES)
            text.lineSequence().filter { it.isNotEmpty() }.forEach { Json.parseToJsonElement(it).jsonObject }
            val omitted = Regex("omitted_lines_at_export_byte_limit=(\\d+)").find(entries.getValue("session-metadata.txt"))!!
            assertTrue(omitted.groupValues[1].toLong() > 0)
        }
    }

    @Test fun snapshotsRemainValidWhileSourceRotatesAndAppendsPartialLines() = sandbox { logs, textLog, cache, _ ->
        val source = log(logs, "main/diagnostics-0.jsonl", record("concurrent", 1))
        val stop = AtomicBoolean(false)
        val failure = AtomicReference<Throwable?>(null)
        val started = CountDownLatch(1)
        val writer = Thread {
            try {
                var sequence = 2
                started.countDown()
                while (!stop.get()) {
                    source.outputStream().use { output ->
                        val line = record("concurrent", sequence++, detail = "世界 content://private/game").toByteArray(Charsets.UTF_8)
                        val split = line.size / 2
                        output.write(line, 0, split)
                        Thread.yield()
                        output.write(line, split, line.size - split)
                    }
                    Files.move(source.toPath(), File(logs, "main/diagnostics-1.jsonl").toPath(), StandardCopyOption.REPLACE_EXISTING)
                }
            } catch (error: Throwable) { failure.set(error) }
        }
        writer.start(); started.await()
        try {
            repeat(4) {
                DiagnosticExport.capture(logs, textLog, cache, "schema=1").use { snapshot ->
                    val entries = archive(snapshot)
                    entries.filterKeys { it.endsWith(".jsonl") }.values.forEach { text ->
                        text.lineSequence().filter { it.isNotBlank() }.forEach { Json.parseToJsonElement(it).jsonObject }
                        assertFalse(text.contains("private"))
                    }
                }
            }
        } finally { stop.set(true); writer.join(5000) }
        assertFalse("Writer did not stop", writer.isAlive)
        failure.get()?.let { throw AssertionError("Source writer failed", it) }
    }
}
