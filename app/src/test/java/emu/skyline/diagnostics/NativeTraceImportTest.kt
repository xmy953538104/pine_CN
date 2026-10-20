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
    @Test fun statusRefinementUsesOneExitAndOneRetryBudgetAcrossPersistedStarts() {
        val root = Files.createTempDirectory("pine-exit-refinement").toFile()
        try {
            val store = ExitRecordStore(root)
            val emitted = mutableListOf<String>()
            val ids = mutableListOf<String>()
            for (status in listOf(0, 6, 6)) store.locked { seen ->
                val stable = ExitRecordIdentity.migrate(seen, 1234567, 5, status, 123, "emulation")
                ids.add(stable)
                val first = "e:$stable" !in seen
                if (first || ExitRecordIdentity.previousStatus(seen, stable) != status) {
                    emitted.add(PreviousProcessExit.eventName(5, !first))
                    ExitRecordIdentity.markObserved(seen, stable, status)
                }
                if (NativeTraceImport.eligible(seen, stable)) NativeTraceImport.attempt(seen, stable, {
                    if (status == 0) NativeTraceSummary.Result("truncated_byte_limit") else NativeTraceSummary.read(NativeTraceSummaryTest().controlledTrace().inputStream(), 123, 10001)
                }) { result, attempt ->
                    assertEquals(if (status == 0) 1 else 2, attempt)
                    assertEquals(if (status == 0) "truncated_byte_limit" else "ok", result.status)
                    true
                }
            }
            assertEquals(1, ids.distinct().size)
            assertEquals(listOf("PREVIOUS_PROCESS_CRASH", "PREVIOUS_PROCESS_EXIT_REFINED"), emitted)
        } finally { root.deleteRecursively() }
    }
    @Test fun legacyZeroAndRefinedStatusMarkersMigrateWithoutThirdCrashOrFreshRetryBudget() {
        val initial = ExitRecordIdentity.legacyId(1234567, 5, 0, 123, "emulation")
        val refined = ExitRecordIdentity.legacyId(1234567, 5, 6, 123, "emulation")
        val seen = linkedSetOf("e:$initial", "r1:$initial:1", "e:$refined", "r1:$refined:1")
        val stable = ExitRecordIdentity.migrate(seen, 1234567, 5, 6, 123, "emulation")
        assertTrue("e:$stable" in seen); assertEquals(6, ExitRecordIdentity.previousStatus(seen, stable))
        assertFalse("e:$initial" in seen); assertFalse("e:$refined" in seen)
        assertTrue(NativeTraceImport.attempt(seen, stable, { NativeTraceSummary.Result("read_unavailable") }) { _, attempt -> assertEquals(3, attempt); true })
        ExitRecordIdentity.migrate(seen, 1234567, 5, 6, 123, "emulation")
        assertFalse(NativeTraceImport.eligible(seen, stable))
        val success = linkedSetOf("e:$initial", "s1:$initial")
        val migrated = ExitRecordIdentity.migrate(success, 1234567, 5, 6, 123, "emulation")
        assertEquals(0, ExitRecordIdentity.previousStatus(success, migrated))
        assertFalse(NativeTraceImport.eligible(success, migrated))
        assertNotEquals(migrated, ExitRecordIdentity.id(1234568, 5, 6, 123, "emulation"))
        assertNotEquals(migrated, ExitRecordIdentity.id(1234567, 5, 6, 124, "emulation"))
    }

    @Test fun alreadyRefinedLegacyExitPublishesOneBridgeAndRetriesFailedPublicationAcrossStarts() {
        val root = Files.createTempDirectory("pine-exit-bridge").toFile()
        try {
            val store = ExitRecordStore(File(root, "private"))
            val initial = ExitRecordIdentity.legacyId(1234567, 5, 0, 123, "emulation")
            val refined = ExitRecordIdentity.legacyId(1234567, 5, 6, 123, "emulation")
            store.locked { seen -> seen.addAll(listOf("e:$initial", "e:$refined", "r1:$initial:1", "r1:$refined:1")) }
            val published = File(root, "accepted.jsonl")
            // First start's rejected admission/failed flush leaves the pending
            // bridge in the real private store, although old keys are removed.
            store.locked { seen ->
                val stable = ExitRecordIdentity.migrate(seen, 1234567, 5, 6, 123, "emulation")
                assertEquals(6, ExitRecordIdentity.previousStatus(seen, stable))
                assertTrue(ExitRecordIdentity.needsObservation(seen, stable, 6))
                assertFalse("e:$initial" in seen); assertFalse("e:$refined" in seen)
                val fields = ExitRecordIdentity.observationFields(seen, stable, 1234567, 5, 6, 123, "emulation")
                assertEquals(stable, fields["exit_record_id"])
                assertEquals(initial, fields["legacy_initial_exit_record_id"])
                assertEquals(refined, fields["legacy_exit_record_id"])
                assertEquals("legacy_status_identity_v1_to_v2", fields["migration_reason"])
                // Deliberately do not markObserved: no accepted+flushed event.
            }
            store.locked { seen ->
                val stable = ExitRecordIdentity.migrate(seen, 1234567, 5, 6, 123, "emulation")
                assertTrue(ExitRecordIdentity.needsObservation(seen, stable, 6))
                val event = PreviousProcessExit.eventName(5, "e:$stable" in seen)
                assertEquals("PREVIOUS_PROCESS_EXIT_REFINED", event)
                val fields = ExitRecordIdentity.observationFields(seen, stable, 1234567, 5, 6, 123, "emulation")
                published.writeText(buildJsonObject {
                    put("event", event); put("fields", JsonObject(fields.mapValues { JsonPrimitive(it.value) }))
                }.toString() + "\n") // Closed before marking publication complete.
                ExitRecordIdentity.markObserved(seen, stable, 6)
                assertTrue(NativeTraceImport.attempt(seen, stable, { NativeTraceSummary.Result("read_unavailable") }) { _, attempt -> assertEquals(3, attempt); true })
            }
            repeat(2) { store.locked { seen ->
                val stable = ExitRecordIdentity.migrate(seen, 1234567, 5, 6, 123, "emulation")
                assertFalse(ExitRecordIdentity.needsObservation(seen, stable, 6))
                assertFalse(NativeTraceImport.eligible(seen, stable))
            } }
            val rows = published.readLines().filter(String::isNotBlank).map { Json.parseToJsonElement(it).jsonObject }
            assertEquals(1, rows.size)
            assertEquals("PREVIOUS_PROCESS_EXIT_REFINED", rows.single()["event"]!!.jsonPrimitive.content)
            val fields = rows.single()["fields"]!!.jsonObject
            assertEquals(initial, fields["legacy_initial_exit_record_id"]!!.jsonPrimitive.content)
            assertEquals(refined, fields["legacy_exit_record_id"]!!.jsonPrimitive.content)
        } finally { root.deleteRecursively() }
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
