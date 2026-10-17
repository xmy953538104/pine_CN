// SPDX-License-Identifier: MPL-2.0
package emu.skyline.diagnostics

import kotlinx.serialization.json.*
import org.junit.Assert.*
import org.junit.Test
import java.io.ByteArrayOutputStream
import java.io.File
import java.nio.file.Files
import java.util.concurrent.TimeoutException
import java.util.zip.ZipInputStream

class NativeTraceImportTest {
    private val id = "b".repeat(64)
    private fun roundtrip(first: () -> NativeTraceSummary.Result) {
        val root = Files.createTempDirectory("pine-trace-retry").toFile()
        try {
            val logs = File(root, "logs").apply { mkdir() }
            val store = ExitRecordStore(File(root, "private"))
            fun write(session: String, suffix: Int, trace: NativeTraceSummary.Result, attempt: Int): Boolean {
                val common = mapOf("source" to "Android_ApplicationExitInfo", "historical_record" to "true", "exit_record_id" to id)
                val fields = NativeTraceImport.fields(common, trace, attempt)
                val row = buildJsonObject {
                    put("session_id", session); put("sequence", 2); put("category", "lifecycle"); put("event", "PREVIOUS_NATIVE_TRACE"); put("level", "INFO")
                    put("fields", JsonObject(fields.mapValues { JsonPrimitive(it.value) }))
                }
                val crash = buildJsonObject {
                    put("session_id", session); put("sequence", 1); put("category", "lifecycle"); put("event", "PREVIOUS_PROCESS_CRASH"); put("level", "ERROR")
                    put("fields", JsonObject(common.mapValues { JsonPrimitive(it.value) }))
                }
                File(logs, "main/diagnostics-$suffix.jsonl").apply { parentFile!!.mkdirs(); writeText("$crash\n$row\n") }
                return true // Files are closed before the import policy records a terminal result.
            }
            store.locked { seen ->
                seen.add("t:$id") // Migrate the earlier candidate's ambiguous negative marker.
                assertTrue(NativeTraceImport.attempt(seen, id, first) { result, attempt -> write("first", 0, result, attempt) })
                assertTrue(NativeTraceImport.eligible(seen, id))
            }
            store.locked { seen ->
                assertTrue(NativeTraceImport.attempt(seen, id, {
                    NativeTraceSummary.read(NativeTraceSummaryTest().controlledTrace().inputStream(), 123, 10001)
                }) { result, attempt ->
                    assertEquals("ok", result.status); assertEquals(2, attempt)
                    write("second", 1, result, attempt)
                })
                assertFalse(NativeTraceImport.eligible(seen, id))
            }
            store.locked { seen -> assertFalse(NativeTraceImport.attempt(seen, id, { error("terminal trace was reread") }) { _, _ -> false }) }
            DiagnosticExport.capture(logs, File(root, "no-text-log"), File(root, "cache"), "test=retry").use { snapshot ->
                val out = ByteArrayOutputStream(); DiagnosticExport.write(out, snapshot)
                val entries = linkedMapOf<String, String>()
                ZipInputStream(out.toByteArray().inputStream()).use { zip ->
                    while (true) { val entry = zip.nextEntry ?: break; entries[entry.name] = zip.readBytes().toString(Charsets.UTF_8) }
                }
                val rows = entries.filterKeys { it.endsWith(".jsonl") }.values.flatMap { it.lineSequence().filter(String::isNotEmpty).toList() }
                    .map { Json.parseToJsonElement(it).jsonObject }
                val traces = rows.filter { it["event"]!!.jsonPrimitive.content == "PREVIOUS_NATIVE_TRACE" }
                assertEquals(2, traces.size)
                assertTrue(traces.any { it["fields"]!!.jsonObject["trace_status"]!!.jsonPrimitive.content == "ok" })
                assertTrue(traces.any { it["fields"]!!.jsonObject["frames"]!!.jsonPrimitive.content.contains("libart.so+0x1234@") })
                assertEquals(1, rows.count { it["event"]!!.jsonPrimitive.content == "PREVIOUS_PROCESS_CRASH" })
                assertTrue(entries.getValue("session-metadata.txt").contains("historical_crash_observations=1"))
            }
        } finally { root.deleteRecursively() }
    }
    @Test fun nullFirstStartThenHealthySecondStartPreservesBothEvidenceAndOneCrash() = roundtrip { NativeTraceSummary.read(null, 123, 10001) }
    @Test fun timeoutFirstStartThenHealthySecondStartPreservesSuccessfulUpgrade() = roundtrip {
        try { PreviousProcessExit.queryBounded(20) { Thread.sleep(500); NativeTraceSummary.Result("unreachable") }; error("Expected deadline") }
        catch (_: TimeoutException) { NativeTraceSummary.Result("observer_deadline") }
    }
    @Test fun transientFailuresAreBoundedAcrossStartsAndFailedWritesNeverBecomeSuccess() {
        val seen = linkedSetOf<String>()
        repeat(3) { attempt ->
            assertTrue(NativeTraceImport.attempt(seen, id, { NativeTraceSummary.Result("read_unavailable") }) { _, number -> assertEquals(attempt + 1, number); true })
        }
        assertFalse(NativeTraceImport.eligible(seen, id))
        assertFalse(seen.any { it.startsWith("s1:") || it.startsWith("p1:") })
        val unwritten = linkedSetOf<String>()
        NativeTraceImport.attempt(unwritten, id, { NativeTraceSummary.Result("ok") }) { _, _ -> false }
        assertTrue(NativeTraceImport.eligible(unwritten, id))
    }
}
