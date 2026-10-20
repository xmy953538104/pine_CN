// SPDX-License-Identifier: MPL-2.0
package emu.skyline.diagnostics

import android.app.Application
import android.app.ApplicationExitInfo
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.shadows.ShadowActivityManager.ApplicationExitInfoBuilder
import java.io.InputStream
import java.io.IOException
import java.nio.file.Files

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, manifest = Config.NONE, sdk = [34])
class SignaledProcessExitTest {
    private val pkg = "io.github.pine.emu.diagnostics"
    private val association = ProcessAssociation("11111111-1111-1111-1111-111111111111",
        "22222222-2222-2222-2222-222222222222", "33333333-3333-3333-3333-333333333333",
        "a".repeat(40), "0100000000000001", "emulation", true, true, "program_header")
    private fun record(reason: Int = 2, status: Int = 11, uid: Int = 10001,
        process: String = "$pkg:emulationProcess", time: Long = 1000) = ApplicationExitInfoBuilder.newBuilder()
        .setReason(reason).setStatus(status).setRealUid(uid).setPid(123).setProcessName(process)
        .setTimestamp(time).setProcessStateSummary(ProcessAssociationCodec.encode(association)).build()
    private data class Row(val name: String, val fields: Map<String, String>)
    private fun fixture(test: (ExitRecordStore) -> Unit) {
        val dir = Files.createTempDirectory("pine-signaled-exit").toFile()
        try { test(ExitRecordStore(dir)) } finally { dir.deleteRecursively() }
    }
    private fun observe(store: ExitRecordStore, records: List<ApplicationExitInfo> = listOf(record()),
        api: Int = 34, timeout: Long = 1500, flush: Boolean = true,
        stream: (ApplicationExitInfo) -> InputStream? = { NativeTraceSummaryTest().controlledTrace().inputStream() }): List<Row> {
        val rows = mutableListOf<Row>()
        PreviousProcessExit.observeRecords(records, 10001, pkg, api, store, stream, timeout,
            { name, _, fields -> rows.add(Row(name, fields)); true }, { flush })
        return rows
    }
    @Test fun actualSignaledObserverPreservesAssociationAndBoundedFields() = fixture { store ->
        val rows = observe(store)
        val fatal = rows.single { it.name == "PREVIOUS_PROCESS_FATAL_SIGNAL" }
        val trace = rows.single { it.name == "PREVIOUS_NATIVE_TRACE" }
        assertEquals("historical_fatal_signal", fatal.fields["exit_classification"])
        assertEquals("ok", trace.fields["trace_status"])
        assertEquals("2", trace.fields["reason_code"]); assertEquals("11", trace.fields["status"])
        assertEquals("1000", trace.fields["exit_wall_clock_ms"])
        assertEquals("host_process_candidate_not_guest", trace.fields["trace_scope"])
        assertEquals(association.session, trace.fields["previous_session_id"])
        assertEquals(association.launch, trace.fields["previous_launch_id"])
        assertEquals(association.process, trace.fields["previous_process_run_id"])
        assertEquals(association.commit, trace.fields["previous_commit"])
        assertEquals(association.title, trace.fields["previous_application_title_id"])
        assertEquals("program_header", trace.fields["previous_application_identity_source"])
        assertEquals(24, trace.fields.size); assertTrue(rows.all { it.fields.size <= 24 })
        assertEquals(fatal.fields["exit_record_id"], trace.fields["exit_record_id"])
        assertFalse(observe(store, stream = { error("terminal reread") }).any { it.name == "PREVIOUS_NATIVE_TRACE" })
    }
    @Test fun normalSignalAndNormalReasonNeverReadOrBecomeFatal() = fixture { store ->
        for ((reason, status) in listOf(2 to 2, 2 to 9, 2 to 15, 1 to 11)) {
            val rows = observe(store, listOf(record(reason, status, time = (reason * 100 + status).toLong())), stream = { error("not a candidate") })
            assertFalse(rows.any { it.name == "PREVIOUS_NATIVE_TRACE" || it.name == "PREVIOUS_PROCESS_FATAL_SIGNAL" || it.name == "PREVIOUS_PROCESS_CRASH" })
        }
        for (status in listOf(4, 6, 7, 8, 11)) assertTrue(PreviousProcessExit.traceCandidate(2, status))
    }
    @Test fun alreadyObservedOldSignaledExitCanImportTraceWithoutReplayingExit() = fixture { store ->
        val id = ExitRecordIdentity.id(1000, 2, 11, 123, "emulation")
        store.locked { ExitRecordIdentity.markObserved(it, id, 11) }
        val rows = observe(store)
        assertFalse(rows.any { it.name == "PREVIOUS_PROCESS_EXIT" || it.name == "PREVIOUS_PROCESS_FATAL_SIGNAL" || it.name == "PREVIOUS_PROCESS_EXIT_REFINED" })
        val fields = rows.single { it.name == "PREVIOUS_NATIVE_TRACE" }.fields
        assertEquals(id, fields["exit_record_id"])
        assertEquals("ok", fields["trace_status"])
    }
    @Test fun foreignUidAndPackageAreRejectedBeforeStreamOrIdentityPersistence() = fixture { store ->
        val rows = observe(store, listOf(record(uid = 10002), record(process = "other:emulationProcess")), stream = { error("foreign read") })
        assertEquals(listOf("PROCESS_EXIT_HISTORY_QUERIED"), rows.map { it.name })
        store.locked { assertTrue(it.isEmpty()) }
    }
    @Test fun actualStoreCapsTransientReadsAcrossStartsAndOnePerObserver() = fixture { store ->
        var reads = 0
        val exits = listOf(record(time = 2000), record(time = 1000))
        repeat(3) {
            val rows = observe(store, exits, stream = { reads++; null })
            assertEquals(1, rows.count { it.name == "PREVIOUS_NATIVE_TRACE" })
            assertEquals("null_or_overwritten", rows.single { it.name == "PREVIOUS_NATIVE_TRACE" }.fields["trace_status"])
        }
        assertEquals(3, reads)
        val next = observe(store, exits, stream = { exit -> assertEquals(1000L, exit.timestamp); reads++; null })
        assertEquals(1, next.count { it.name == "PREVIOUS_NATIVE_TRACE" }); assertEquals(4, reads)
        store.locked { seen ->
            val id = ExitRecordIdentity.id(2000, 2, 11, 123, "emulation")
            assertFalse(NativeTraceImport.eligible(seen, id))
        }
    }
    @Test fun unsupportedApiDoesNotCallRetriever() = fixture { store ->
        val rows = observe(store, api = 30, stream = { error("API30 getter") })
        assertEquals("android_api_below_31", rows.single { it.name == "PREVIOUS_NATIVE_TRACE" }.fields["trace_status"])
    }
    @Test fun nonProtobufAndIoNeverProduceFrames() {
        for (stream in listOf<() -> InputStream>({ "ANR text, not a tombstone".byteInputStream() },
            { object : InputStream() { override fun read(): Int = throw IOException("private path") } })) fixture { store ->
            val fields = observe(store, stream = { stream() }).single { it.name == "PREVIOUS_NATIVE_TRACE" }.fields
            assertNotEquals("ok", fields["trace_status"]); assertEquals("", fields["frames"])
            assertFalse(fields.toString().contains("private path"))
        }
    }
    @Test fun realWorkerDeadlineIsUnavailableAndDoesNotInventFrames() = fixture { store ->
        val fields = observe(store, timeout = 20, stream = { Thread.sleep(1000); null })
            .single { it.name == "PREVIOUS_NATIVE_TRACE" }.fields
        assertEquals("observer_deadline", fields["trace_status"]); assertEquals("", fields["frames"])
    }
    @Test fun failedFlushAllowsSafeRetryButCannotBypassAttemptBudget() = fixture { store ->
        repeat(3) { observe(store, flush = false) }
        assertFalse(observe(store, stream = { error("fourth attempt") }).any { it.name == "PREVIOUS_NATIVE_TRACE" })
    }
    @Test fun statusRefinementKeepsSameExitIdentityAndDoesNotMakeSecondCrash() = fixture { store ->
        val initial = observe(store, listOf(record(status = 0)), stream = { error("unknown signal") })
            .single { it.name == "PREVIOUS_PROCESS_EXIT" }
        val later = observe(store)
        val refined = later.single { it.name == "PREVIOUS_PROCESS_EXIT_REFINED" }
        assertEquals(initial.fields["exit_record_id"], refined.fields["exit_record_id"])
        assertEquals("historical_fatal_signal", refined.fields["exit_classification"])
        assertFalse(later.any { it.name == "PREVIOUS_PROCESS_FATAL_SIGNAL" || it.name == "PREVIOUS_PROCESS_CRASH" })
        assertEquals(1, later.count { it.name == "PREVIOUS_NATIVE_TRACE" })
    }
    @Test fun protobufOwnershipMismatchIsTerminalEvenForEligibleSystemExit() = fixture { store ->
        val bad = NativeTraceSummaryTest().controlledTrace()
        // The system record PID is deliberately different from the protobuf PID 123.
        val exit = ApplicationExitInfoBuilder.newBuilder().setReason(2).setStatus(11).setRealUid(10001)
            .setPid(124).setProcessName(pkg).setTimestamp(1000).build()
        val fields = observe(store, listOf(exit), stream = { bad.inputStream() }).single { it.name == "PREVIOUS_NATIVE_TRACE" }.fields
        assertEquals("ownership_mismatch", fields["trace_status"]); assertEquals("", fields["frames"])
        assertFalse(observe(store, listOf(exit), stream = { error("mismatched reread") }).any { it.name == "PREVIOUS_NATIVE_TRACE" })
    }
    @Test fun defaultSystemRetrieverFlowsThroughRealObserverAndStore() = fixture { store ->
        val exit = ApplicationExitInfoBuilder.newBuilder().setReason(2).setStatus(11).setRealUid(10001)
            .setPid(123).setProcessName(pkg).setTimestamp(1000)
            .setTraceInputStream(NativeTraceSummaryTest().controlledTrace().inputStream()).build()
        val rows = mutableListOf<Row>()
        PreviousProcessExit.observeRecords(listOf(exit), 10001, pkg, 34, store,
            publish = { name, _, fields -> rows.add(Row(name, fields)); true }, flush = { true })
        assertEquals("ok", rows.single { it.name == "PREVIOUS_NATIVE_TRACE" }.fields["trace_status"])
    }

    @Test fun nativeCandidateWinsOneReadWithoutReorderingExitEventsOrMergingIdentity() = fixture { store ->
        val exits = listOf(record(time = 2000), record(reason = 5, status = 0, time = 1000))
        val rows = observe(store, exits, stream = { exit ->
            assertEquals(5, exit.reason); NativeTraceSummaryTest().controlledTrace().inputStream()
        })
        assertEquals(listOf("PREVIOUS_PROCESS_FATAL_SIGNAL", "PREVIOUS_PROCESS_CRASH"),
            rows.filter { it.name == "PREVIOUS_PROCESS_FATAL_SIGNAL" || it.name == "PREVIOUS_PROCESS_CRASH" }.map { it.name })
        val exitIds = rows.filter { it.name == "PREVIOUS_PROCESS_FATAL_SIGNAL" || it.name == "PREVIOUS_PROCESS_CRASH" }
            .map { it.fields["exit_record_id"] }
        assertEquals(2, exitIds.distinct().size)
        assertEquals("5", rows.single { it.name == "PREVIOUS_NATIVE_TRACE" }.fields["reason_code"])
        store.locked { seen ->
            val signaledId = ExitRecordIdentity.id(2000, 2, 11, 123, "emulation")
            assertFalse(seen.any { it.startsWith("r1:$signaledId:") })
        }
        val later = observe(store, exits, stream = { exit -> assertEquals(2, exit.reason); null })
        assertEquals("2", later.single { it.name == "PREVIOUS_NATIVE_TRACE" }.fields["reason_code"])
    }
    @Test fun nativeTransientExhaustionThenFallsBackWithinUnchangedBudget() = fixture { store ->
        val exits = listOf(record(time = 2000), record(reason = 5, status = 0, time = 1000))
        repeat(3) {
            assertEquals("5", observe(store, exits, stream = { exit -> assertEquals(5, exit.reason); null })
                .single { it.name == "PREVIOUS_NATIVE_TRACE" }.fields["reason_code"])
        }
        assertEquals("2", observe(store, exits, stream = { exit -> assertEquals(2, exit.reason); null })
            .single { it.name == "PREVIOUS_NATIVE_TRACE" }.fields["reason_code"])
    }
    @Test fun migratedLegacyNativeTerminalAndExhaustionDoNotBlockSignaledFallback() {
        for (marker in listOf("s1", "p1", "r1")) fixture { store ->
            val legacy = ExitRecordIdentity.legacyId(1000, 5, 0, 123, "emulation")
            store.locked { seen -> seen.add(if (marker == "r1") "r1:$legacy:3" else "$marker:$legacy") }
            val rows = observe(store, listOf(record(time = 2000), record(reason = 5, status = 0)),
                stream = { exit -> assertEquals(2, exit.reason); null })
            assertEquals("2", rows.single { it.name == "PREVIOUS_NATIVE_TRACE" }.fields["reason_code"])
            store.locked { seen -> assertFalse(seen.any { it.contains(legacy) }) }
        }
    }
    @Test fun priorityKeepsFourRecordLimitOwnershipAndNewestNativeWithinGroup() = fixture { store ->
        val rows = observe(store, listOf(record(time = 5000), record(reason = 5, time = 4000, uid = 10002),
            record(reason = 5, time = 2000), record(reason = 5, time = 3000), record(reason = 5, time = 9000)),
            stream = { exit -> assertEquals(3000L, exit.timestamp); null })
        assertEquals("3", rows.single { it.name == "PROCESS_EXIT_HISTORY_QUERIED" }.fields["records"])
        assertEquals("3000", rows.single { it.name == "PREVIOUS_NATIVE_TRACE" }.fields["exit_wall_clock_ms"])
    }

}
