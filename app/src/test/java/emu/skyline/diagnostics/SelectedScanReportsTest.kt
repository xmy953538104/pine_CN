// SPDX-License-Identifier: MPL-2.0
package emu.skyline.diagnostics

import android.app.Application
import android.content.Context
import android.content.ContextWrapper
import android.net.Uri
import emu.skyline.data.BaseAppItem
import emu.skyline.loader.*
import kotlinx.serialization.json.*
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import org.robolectric.annotation.Implementation
import org.robolectric.annotation.Implements
import org.robolectric.util.ReflectionHelpers
import java.io.ByteArrayOutputStream
import java.io.File
import java.nio.file.Files
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.CountDownLatch
import java.util.concurrent.ThreadPoolExecutor
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import java.util.zip.ZipInputStream

/** Real attempt/session/store/export code; native sink methods are explicit host substitutes. */
@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, manifest = Config.NONE, sdk = [29],
    instrumentedPackages = ["emu.skyline.diagnostics.Diagnostics"], shadows = [SelectedScanReportsTest.NativeDiagnostics::class])
class SelectedScanReportsTest {
    data class Event(val name: String, val fields: Map<String, String>)
    @Implements(value = Diagnostics::class, isInAndroidSdk = false)
    class NativeDiagnostics {
        companion object {
            @JvmStatic @Implementation fun configureNative(path: String, enabled: Boolean, verbose: Boolean,
                files: Int, session: String, categories: String, process: String): Boolean { nativeActive = enabled; return enabled }
            @JvmStatic @Implementation fun statusNative() = longArrayOf(if (nativeActive) 1 else 0, 0, 0, 0)
            @JvmStatic @Implementation fun setLaunchNative(id: String, title: String, newLaunch: Boolean) = Unit
            @JvmStatic @Implementation fun emitFieldsNative(category: String, event: String, level: String, keys: Array<String>, values: Array<String>): Boolean {
                events.add(Event(event, keys.zip(values).toMap())); return true
            }
            @JvmStatic @Implementation fun emitNative(category: String, event: String, detail: String, title: String) = Unit
            @JvmStatic @Implementation fun stopNative() { nativeActive = false }
            @JvmStatic @Implementation fun flushNative() = true
        }
    }
    companion object {
        @Volatile private var nativeActive = false
        val events = CopyOnWriteArrayList<Event>()
    }
    private val app: Context get() = RuntimeEnvironment.getApplication()
    @Before fun reset() {
        Diagnostics.finishSession(); SelectedScanReports.awaitIdle(); events.clear()
        File(app.filesDir, "diagnostics").deleteRecursively()
        Diagnostics.preferences(app).edit().clear().commit()
    }
    @After fun stop() { Diagnostics.finishSession(); SelectedScanReports.awaitIdle() }
    private fun enable(): SelectedScanSession {
        Diagnostics.preferences(app).edit().putBoolean("enabled", true).commit(); Diagnostics.apply(app)
        return requireNotNull(Diagnostics.selectedScanSession())
    }
    private fun records(): List<JsonObject> = SelectedScanRecord.names.flatMap { name ->
        val file = File(File(app.filesDir, "diagnostics"), name)
        if (!file.exists()) emptyList() else file.readLines().filter { it.isNotBlank() }.map { Json.parseToJsonElement(it).jsonObject }
    }

    @Test fun defaultOffCreatesNoReportIoOrPublisherCalls() {
        val filesLookups = AtomicInteger()
        val context = object : ContextWrapper(app) {
            override fun getApplicationContext(): Context = this
            override fun getFilesDir(): File { filesLookups.incrementAndGet(); error("OFF must not inspect paths") }
        }
        repeat(40) {
            val attempt = SelectedScanAttempt(context, SelectedContentProfile.Modern40, publish = { _, _, _ -> error("OFF publication") })
            assertTrue(attempt.finish("completed", SelectedScanUi("no_groups", "selected_no_candidates", "primary_groups")))
        }
        SelectedScanReports.submit(context, SelectedScanSession("00000000-0000-0000-0000-000000000001"), SelectedScanTestFixture.fields())
        assertEquals(0, filesLookups.get()); assertTrue(records().isEmpty())
    }

    @Test fun defaultOffV2KeyReportCannotResolvePathsOrCreateWriterWork() {
        val context = object : ContextWrapper(app) {
            override fun getApplicationContext(): Context = this
            override fun getFilesDir(): File = error("OFF must not resolve paths for v2")
        }
        val native = SelectedNativeScanReport.sanitize(SelectedKeyStateFixture.loadedReport())
        val attempt = SelectedScanAttempt(context, SelectedContentProfile.Modern40, publish = { _, _, _ -> error("OFF publication") })
        assertTrue(attempt.finish("completed", primaryState(SelectedPreviewFixture.catalog()), native))
        assertFalse(attempt.finish("cancelled", SelectedScanUi("cancelled", "selected_scan_caveat", "scan"), native))
        SelectedScanReports.submit(context, SelectedScanSession("00000000-0000-0000-0000-000000000001"), SelectedScanTestFixture.fields(native.safe))
        assertTrue(SelectedScanReports.awaitIdle()); assertTrue(records().isEmpty())
    }

    @Test fun oneTerminalAndSafeMonotonicFieldsArePreservedEvenIfPublisherFails() {
        val captured = mutableListOf<JsonObject>(); var clock = 100L
        val attempt = SelectedScanAttempt(app, SelectedContentProfile.Legacy48, SelectedScanSession("synthetic"), { clock }) { _, _, fields -> captured.add(fields) }
        clock = 140
        assertTrue(attempt.finish("cancelled", SelectedScanUi("cancelled", "selected_scan_caveat", "scan"), failureCode = "user_cancelled"))
        assertFalse(attempt.finish("completed", SelectedScanUi("no_groups", "selected_no_candidates", "primary_groups")))
        assertFalse(attempt.finish("failed", SelectedScanUi("scan_failed", "selected_scan_failed", "scan")))
        assertEquals(1, captured.size); assertEquals(40, captured.single().getValue("duration_us").jsonPrimitive.int)
        assertEquals("legacy-48", captured.single().getValue("profile").jsonPrimitive.content)
        assertTrue(Regex("[0-9a-f-]{36}").matches(captured.single().getValue("scan_attempt_id").jsonPrimitive.content))
        val broken = SelectedScanAttempt(app, SelectedContentProfile.Modern40, SelectedScanSession("synthetic"), publish = { _, _, _ -> throw IllegalStateException("PRIVATE_CANARY") })
        assertTrue(broken.finish("failed", SelectedScanUi("scan_failed", "selected_scan_failed", "scan")))
    }

    @Test fun realSessionWriteAndSmallLiveEventUseIndependentReportSequence() {
        val session = enable()
        val attempt = SelectedScanAttempt(app, SelectedContentProfile.Modern40)
        assertTrue(attempt.finish("completed", primaryState(SelectedPreviewFixture.catalog()), SelectedNativeScanReport.sanitize(SelectedScanTestFixture.report())))
        assertTrue(SelectedScanReports.awaitIdle())
        val record = records().single()
        assertEquals(session.id, record.getValue("session_id").jsonPrimitive.content)
        assertTrue(record.getValue("report_sequence").jsonPrimitive.long > 0); assertFalse("sequence" in record)
        val event = events.single { it.name == "SELECTED_SCAN_FINISHED" }
        assertEquals(attempt.id, event.fields["scan_attempt_id"]); assertEquals("completed", event.fields["terminal_status"])
        assertEquals("missing_header_key", event.fields["first_native_issue"])
        assertEquals("true", event.fields["report_persisted"]); assertTrue(event.fields.size <= 12)
        assertFalse(event.fields.values.any { it.contains("catalog_kind") })
    }

    @Test fun boundedQueueDropsAreVisibleAndQueuedRevokedSessionCannotWrite() {
        val session = enable()
        val executor = ReflectionHelpers.callInstanceMethod<ThreadPoolExecutor>(SelectedScanReports, "getExecutor")
        val entered = CountDownLatch(1); val release = CountDownLatch(1)
        executor.execute { entered.countDown(); release.await(5, TimeUnit.SECONDS) }
        try {
            assertTrue(entered.await(2, TimeUnit.SECONDS))
            repeat(12) { SelectedScanReports.submit(app, session, SelectedScanTestFixture.fields()) }
            assertTrue(executor.queue.size <= 8)
            Diagnostics.finishSession() // Must not wait for the occupied worker or file I/O.
        } finally { release.countDown() }
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5)
        while (executor.queue.isNotEmpty() && System.nanoTime() < deadline) Thread.yield()
        assertTrue(executor.queue.isEmpty())
        assertTrue(SelectedScanReports.awaitIdle()); assertTrue(records().isEmpty())
        val replacement = enable(); assertNotEquals(session, replacement)
        SelectedScanReports.submit(app, replacement, SelectedScanTestFixture.fields())
        assertTrue(SelectedScanReports.awaitIdle())
        assertTrue(records().single().getValue("fields").jsonObject.getValue("dropped_report_count").jsonPrimitive.long >= 4)
    }

    @Test fun ordinaryMetadataHintRecordsActualResourceAndNeverPretendsToScan() {
        enable(); events.clear()
        fun item(result: LoaderResult) = BaseAppItem(AppEntry(name = "private-name", version = null, titleId = null,
            addOnContentBaseId = null, author = null, icon = null, romType = RomType.Unknown, parentTitleId = null,
            format = RomFormat.NSP, uri = Uri.parse("content://PRIVATE_CANARY/file"), loaderResult = result))
        val ordinary = item(LoaderResult.Success)
        assertEquals(app.getString(emu.skyline.R.string.metadata_missing), appDetailsVersion(app, ordinary, null))
        assertEquals("version supplied", appDetailsVersion(app, ordinary, "version supplied"))
        appDetailsVersion(app, item(LoaderResult.MissingTitleKey), null)
        val observed = events.filter { it.name == "APP_DETAILS_METADATA_HINT_SHOWN" }
        assertEquals(listOf("metadata_missing", "missing_title_key"), observed.map { it.fields.getValue("ui_resource_name") })
        assertTrue(observed.all { it.fields["ui_stage"] == "app_details_version" && it.fields["scan_performed"] == "false" })
        assertFalse(observed.toString().contains("PRIVATE_CANARY")); assertTrue(records().isEmpty())
    }

    private fun sandbox(test: (File) -> Unit) {
        val root = Files.createTempDirectory("pine-selected-scan-log").toFile()
        try { test(root) } finally { root.deleteRecursively() }
    }
    private fun archive(snapshot: DiagnosticExport.Snapshot): Map<String, String> {
        val bytes = ByteArrayOutputStream(); DiagnosticExport.write(bytes, snapshot)
        val entries = linkedMapOf<String, String>()
        ZipInputStream(bytes.toByteArray().inputStream()).use { zip ->
            while (true) { val entry = zip.nextEntry ?: break; entries[entry.name] = zip.readBytes().toString(Charsets.UTF_8) }
        }
        return entries
    }

    @Test fun rotationAndRecordLimitsKeepExactlyTwoBoundedRegularFiles() = sandbox { root ->
        val dir = File(root, "diagnostics/main"); val writer = SelectedScanLogWriter(dir)
        repeat(150) { assertTrue(writer.append(SelectedScanTestFixture.record(it + 1L))) }
        assertEquals(setOf("selected-scan-0.jsonl", "selected-scan-1.jsonl"), dir.listFiles()!!.map { it.name }.toSet())
        for (file in dir.listFiles()!!) {
            assertTrue(file.length() <= SelectedScanRecord.MaxFileBytes)
            val lines = file.readLines(); assertTrue(lines.size <= SelectedScanRecord.MaxRecords)
            assertTrue(lines.all { it.toByteArray().size + 1 <= SelectedScanRecord.MaxRecordBytes })
        }
        val large = JsonObject(SelectedScanTestFixture.record() + ("private" to JsonPrimitive("x".repeat(SelectedScanRecord.MaxRecordBytes))))
        assertFalse(writer.append(large))
    }

    @Test fun unsafeDestinationAndOversizedExistingFileAreRejectedWithoutFollowing() = sandbox { root ->
        val dir = File(root, "diagnostics/main").apply { mkdirs() }
        val outside = File(root, "outside").apply { writeText("PRIVATE_CANARY") }
        Files.createSymbolicLink(File(dir, "selected-scan-0.jsonl").toPath(), outside.toPath())
        assertFalse(SelectedScanLogWriter(dir).append(SelectedScanTestFixture.record()))
        assertEquals("PRIVATE_CANARY", outside.readText())
        File(dir, "selected-scan-0.jsonl").delete()
        File(dir, "selected-scan-0.jsonl").mkdir()
        assertFalse(SelectedScanLogWriter(dir).append(SelectedScanTestFixture.record()))
        File(dir, "selected-scan-0.jsonl").delete()
        File(dir, "selected-scan-0.jsonl").writeText("x".repeat(SelectedScanRecord.MaxFileBytes + 1))
        assertFalse(SelectedScanLogWriter(dir).append(SelectedScanTestFixture.record()))
    }

    @Test fun fixedExportPreviewRestoreCleanupAndDistinctSequenceAreComplete() = sandbox { root ->
        val logs = File(root, "diagnostics").apply { mkdir() }; val main = File(logs, "main").apply { mkdir() }
        val safe = SelectedScanTestFixture.record()
        val polluted = JsonObject(safe + mapOf("catalog" to SelectedPreviewFixture.json(), "private" to JsonPrimitive("PRIVATE_CANARY")))
        File(main, "selected-scan-0.jsonl").writeText(polluted.toString() + "\n")
        File(main, "selected-scan-1.jsonl").writeText(safe.toString() + "\n")
        File(main, "selected-scan-2.jsonl").writeText("PRIVATE_CANARY")
        File(logs, "emulation").mkdir(); File(logs, "emulation/selected-scan-0.jsonl").writeText("PRIVATE_CANARY")
        File(main, "diagnostics-0.jsonl").writeText(buildJsonObject {
            put("schema", 1); put("session_id", safe.getValue("session_id")); put("sequence", 1); put("category", "content")
            put("event", "BASE_SELECTED"); put("level", "INFO"); put("fields", buildJsonObject { put("stage", "content_ready") })
        }.toString() + "\n")
        val cache = File(root, "cache")
        val snapshot = DiagnosticExport.capture(logs, File(root, "missing.log"), cache, "schema=1")
        val result = archive(snapshot)
        assertEquals(setOf("session-metadata.txt", "main/diagnostics-0.jsonl", "main/selected-scan-0.jsonl", "main/selected-scan-1.jsonl"), result.keys)
        assertFalse(result.values.any { it.contains("PRIVATE_CANARY") || it.contains("catalog_kind") })
        assertTrue(result.getValue("session-metadata.txt").contains("exported_records=2"))
        assertTrue(result.getValue("session-metadata.txt").contains("omitted_duplicate_records=1"))
        assertTrue(snapshot.entries.all { it.bytes <= if (it.name.contains("selected-scan")) SelectedScanRecord.MaxFileBytes else DiagnosticExport.MAX_METADATA_BYTES })
        val restored = requireNotNull(DiagnosticExport.restore(cache, snapshot.id))
        assertEquals(result, archive(restored)); restored.close(); snapshot.close()
        assertFalse(File(cache, snapshot.id).exists())
    }

    @Test fun historicalV1AndV2KeyStateShareBoundedExportRestoreAndPrivacyRebuild() = sandbox { root ->
        val logs = File(root, "logs").apply { mkdir() }; val main = File(logs, "main").apply { mkdir() }
        val old = SelectedScanTestFixture.record()
        val report = SelectedKeyStateFixture.loadedReport()
        val state = report.getValue("key_state").jsonObject
        val polluted = JsonObject(report + mapOf("catalog" to buildJsonObject { put("private", "PRIVATE_CANARY") },
            "key_state" to JsonObject(state + mapOf("path" to JsonPrimitive("content://PRIVATE_CANARY/file"),
                "line" to JsonPrimitive("prod.keys PRIVATE_CANARY"), "rights_id" to JsonPrimitive("PRIVATE_CANARY")))))
        val newest = SelectedScanTestFixture.record(2, SelectedScanTestFixture.fields(polluted))
        val writer = SelectedScanLogWriter(main)
        assertTrue(writer.append(old)); assertTrue(writer.append(newest))
        File(main, "selected-scan-1.jsonl").writeText(newest.toString() + "\n") // Historical duplicate across rotation.
        val cache = File(root, "cache")
        val snapshot = DiagnosticExport.capture(logs, File(root, "absent.log"), cache, "schema=1")
        val exported = archive(snapshot)
        val rows = exported.filterKeys { it in SelectedScanRecord.names }.values.flatMap { text -> text.lines().filter(String::isNotBlank) }
            .map { Json.parseToJsonElement(it).jsonObject }
        assertEquals(2, rows.size)
        val reports = rows.map { it.getValue("fields").jsonObject.getValue("native_report").jsonObject }
        assertEquals(listOf(1, 2), reports.map { it.getValue("schema_version").jsonPrimitive.int })
        assertEquals(SelectedScanTestFixture.report(), reports[0])
        assertEquals(report, reports[1])
        assertEquals("not_matched", reports[1].getValue("catalog_summary").jsonObject.getValue("members").jsonArray.single().jsonObject.getValue("title_key_lookup").jsonPrimitive.content)
        assertFalse(exported.values.any { it.contains("PRIVATE_CANARY") || it.contains("REDACTED_CREDENTIAL_LINE") || it.contains("catalog_kind") })
        assertTrue(snapshot.entries.filter { it.name in SelectedScanRecord.names }.all { it.bytes <= SelectedScanRecord.MaxFileBytes })
        assertEquals(setOf("main/selected-scan-0.jsonl", "main/selected-scan-1.jsonl"), exported.keys.filter { it.startsWith("main/selected-scan") }.toSet())
        val restored = requireNotNull(DiagnosticExport.restore(cache, snapshot.id))
        assertEquals(exported, archive(restored)); restored.close(); snapshot.close()
        assertFalse(File(cache, snapshot.id).exists())
    }

    @Test fun exportRejectsSelectedNativeSequenceOversizedLineFileAndUnsafeSource() = sandbox { root ->
        val logs = File(root, "logs").apply { mkdir() }; val main = File(logs, "main").apply { mkdir() }
        val record = SelectedScanTestFixture.record()
        File(main, "selected-scan-0.jsonl").writeText(JsonObject(record + ("sequence" to JsonPrimitive(2))).toString() + "\n" +
            "x".repeat(SelectedScanRecord.MaxRecordBytes) + "\n")
        File(main, "selected-scan-1.jsonl").writeText("x".repeat(SelectedScanRecord.MaxFileBytes + 1))
        DiagnosticExport.capture(logs, File(root, "missing.log"), File(root, "cache"), "schema=1").use { snapshot ->
            val result = archive(snapshot)
            assertEquals("", result.getValue("main/selected-scan-0.jsonl"))
            assertFalse("main/selected-scan-1.jsonl" in result)
            assertTrue(result.getValue("session-metadata.txt").contains("omitted_oversized_lines=1"))
        }
        File(main, "selected-scan-0.jsonl").delete(); File(main, "selected-scan-1.jsonl").delete()
        Files.createSymbolicLink(File(main, "selected-scan-0.jsonl").toPath(), File(root, "private").apply { writeText("PRIVATE_CANARY") }.toPath())
        DiagnosticExport.capture(logs, File(root, "missing.log"), File(root, "cache-unsafe"), "schema=1").use { assertFalse("main/selected-scan-0.jsonl" in archive(it)) }
    }
}
