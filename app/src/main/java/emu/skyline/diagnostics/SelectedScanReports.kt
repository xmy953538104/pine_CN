// SPDX-License-Identifier: MPL-2.0
package emu.skyline.diagnostics

import android.content.Context
import android.os.SystemClock
import emu.skyline.BuildConfig
import emu.skyline.loader.SelectedContentProfile
import emu.skyline.loader.SelectedGroupCandidate
import emu.skyline.loader.SelectedNativeScanReport
import emu.skyline.loader.SelectedRejection
import emu.skyline.loader.checkJsonEnvelope
import emu.skyline.loader.selectedScanPredicates
import kotlinx.serialization.json.*
import java.io.File
import java.nio.ByteBuffer
import java.nio.channels.FileChannel
import java.nio.file.Files
import java.nio.file.LinkOption
import java.nio.file.StandardCopyOption
import java.nio.file.StandardOpenOption
import java.util.UUID
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.CountDownLatch
import java.util.concurrent.ThreadPoolExecutor
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong

internal data class SelectedScanSession(val id: String)
internal data class SelectedScanUi(val outcome: String, val resource: String, val stage: String,
    val primaryRows: List<SelectedGroupCandidate> = emptyList())

/** The attempt owns a single terminal; a cancelled blocking JNI result cannot publish later. */
internal class SelectedScanAttempt(private val context: Context, val profile: SelectedContentProfile,
    private val session: SelectedScanSession? = Diagnostics.selectedScanSession(),
    private val clock: () -> Long = { SystemClock.elapsedRealtimeNanos() / 1000 },
    private val publish: (Context, SelectedScanSession, JsonObject) -> Unit = SelectedScanReports::submit) {
    val id: String = UUID.randomUUID().toString()
    private val started = maxOf(0, clock())
    private val terminal = AtomicBoolean(false)
    fun finish(status: String, ui: SelectedScanUi, native: SelectedNativeScanReport? = null, failureCode: String? = null): Boolean {
        require(status in setOf("completed", "failed", "cancelled"))
        if (!terminal.compareAndSet(false, true)) return false
        val captured = session
        if (captured != null) {
            val elapsed = maxOf(0, clock() - started)
            val fields = buildJsonObject {
                put("scan_attempt_id", id); put("profile", profile.wire); put("terminal_status", status)
                put("started_elapsed_us", started); put("duration_us", elapsed)
                put("ui_stage", ui.stage); put("ui_resource_name", ui.resource); put("ui_outcome", ui.outcome)
                put("failure_code", failureCode?.let(::JsonPrimitive) ?: JsonNull)
                put("first_rejection_predicate", ui.primaryRows.firstOrNull { it.rejection != null }?.predicate?.let(::JsonPrimitive) ?: JsonNull)
                put("primary_rows", JsonArray(ui.primaryRows.take(32).map { row -> buildJsonObject {
                    put("ordinal", row.ordinal); put("rejection", row.rejection?.name?.let(::JsonPrimitive) ?: JsonNull)
                    put("predicate", row.predicate?.let(::JsonPrimitive) ?: JsonNull)
                } }))
                put("primary_rows_omitted", maxOf(0, ui.primaryRows.size - 32))
                put("build", buildJsonObject {
                    put("application_id", BuildConfig.APPLICATION_ID); put("version_code", BuildConfig.VERSION_CODE)
                    put("version_name", BuildConfig.VERSION_NAME); put("build_type", BuildConfig.BUILD_TYPE)
                    put("commit", BuildConfig.DIAGNOSTICS_COMMIT); put("selected_content_enabled", BuildConfig.SELECTED_CONTENT_ENABLED)
                })
                put("native_report", native?.safe ?: JsonNull)
            }
            runCatching { publish(context.applicationContext, captured, fields) }
        }
        return true
    }
}

/** Fixed parser for persisted records as well as new writes; unknown keys never survive export. */
internal object SelectedScanRecord {
    const val MaxRecordBytes = 32 * 1024
    const val MaxFileBytes = 128 * 1024
    const val MaxRecords = 64
    val names = listOf("main/selected-scan-0.jsonl", "main/selected-scan-1.jsonl")
    val stages = setOf("scan", "primary_groups", "programs", "updates", "aocs", "review")
    val resources = setOf("selected_scan_caveat", "selected_scan_failed", "selected_no_candidates", "selected_primary_all_rejected",
        "selected_primary_title", "selected_program_title", "selected_update_title", "selected_aoc_title", "selected_review")
    val outcomes = setOf("no_groups", "no_primary_role", "primary_rows_all_rejected", "primary_rows_available", "scan_failed", "cancelled")
    val failures = setOf("observed_json_limit", "observed_json_invalid", "observed_report_invalid", "catalog_json_invalid",
        "input_unavailable", "input_permission_denied", "java_inspection_failed", "native_entry_unavailable", "user_cancelled", "view_destroyed")
    private val uuid = Regex("[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}")
    private val packageId = Regex("[A-Za-z][A-Za-z0-9_]*(?:\\.[A-Za-z][A-Za-z0-9_]*){1,15}")
    private val version = Regex("[0-9][A-Za-z0-9._+\\-]{0,79}")
    private val commit = Regex("[0-9a-fA-F]{7,40}(?:-dirty)?")

    fun create(session: SelectedScanSession, sequence: Long, fields: JsonObject): JsonObject = requireNotNull(sanitize(buildJsonObject {
        put("schema", 1); put("record_kind", "selected_scan_report"); put("category", "content")
        put("event", "SELECTED_SCAN_REPORT"); put("level", "INFO"); put("session_id", session.id)
        put("process_scope", "main"); put("report_sequence", sequence); put("fields", fields)
    }))

    fun sanitize(input: JsonObject): JsonObject? = runCatching {
        require(input.toString().toByteArray(Charsets.UTF_8).size + 1 <= MaxRecordBytes)
        require(input.recordLong("schema") == 1L && input.recordText("record_kind") == "selected_scan_report")
        require(input.recordText("category") == "content" && input.recordText("event") == "SELECTED_SCAN_REPORT")
        require(input.recordText("level") == "INFO" && input.recordText("process_scope") == "main" && "sequence" !in input)
        val session = input.recordText("session_id").also { require(uuid.matches(it)) }
        val sequence = input.recordLong("report_sequence").also { require(it > 0) }
        val fields = input.getValue("fields") as JsonObject
        val build = fields.getValue("build") as JsonObject
        val rows = fields.getValue("primary_rows") as JsonArray
        require(rows.size <= 32)
        val safeFields = buildJsonObject {
            put("scan_attempt_id", fields.recordText("scan_attempt_id").also { require(uuid.matches(it)) })
            put("profile", fields.recordText("profile").also { require(it in setOf("modern-40", "legacy-48")) })
            put("terminal_status", fields.recordText("terminal_status").also { require(it in setOf("completed", "failed", "cancelled")) })
            put("started_elapsed_us", fields.recordLong("started_elapsed_us")); put("duration_us", fields.recordLong("duration_us"))
            put("dropped_report_count", fields.recordLong("dropped_report_count"))
            put("ui_stage", fields.recordText("ui_stage").also { require(it in stages) })
            put("ui_resource_name", fields.recordText("ui_resource_name").also { require(it in resources) })
            put("ui_outcome", fields.recordText("ui_outcome").also { require(it in outcomes) })
            put("failure_code", fields["failure_code"].safeNullable(failures + SelectedNativeScanReport.codes, "java_inspection_failed"))
            put("first_rejection_predicate", fields["first_rejection_predicate"].safeNullable(selectedScanPredicates, "unknown_ui_predicate"))
            put("primary_rows", JsonArray(rows.map { raw ->
                val row = raw as JsonObject
                buildJsonObject {
                    put("ordinal", row.recordCount("ordinal"))
                    put("rejection", row["rejection"].safeNullable(SelectedRejection.values().map { it.name }.toSet(), "UnknownRole"))
                    put("predicate", row["predicate"].safeNullable(selectedScanPredicates, "unknown_ui_predicate"))
                }
            }))
            put("primary_rows_omitted", fields.recordCount("primary_rows_omitted"))
            put("build", buildJsonObject {
                put("application_id", build.recordText("application_id").let { if (it.length <= 160 && packageId.matches(it)) it else "unknown" })
                put("version_code", build.recordCount("version_code"))
                put("version_name", build.recordText("version_name").let { if (version.matches(it)) it else "unknown" })
                put("build_type", build.recordText("build_type").let { if (it in setOf("debug", "release")) it else "unknown" })
                put("commit", build.recordText("commit").let { if (commit.matches(it)) it else "unknown" })
                put("selected_content_enabled", (build.getValue("selected_content_enabled") as JsonPrimitive).also { require(!it.isString && it.booleanOrNull != null) })
            })
            val native = fields.getValue("native_report")
            put("native_report", if (native == JsonNull) JsonNull else SelectedNativeScanReport.sanitize(native as JsonObject).safe)
        }
        buildJsonObject {
            put("schema", 1); put("record_kind", "selected_scan_report"); put("category", "content"); put("event", "SELECTED_SCAN_REPORT")
            put("level", "INFO"); put("session_id", session); put("process_scope", "main"); put("report_sequence", sequence); put("fields", safeFields)
        }.also { require(it.toString().toByteArray(Charsets.UTF_8).size + 1 <= MaxRecordBytes) }
    }.getOrNull()

    fun parseLine(text: String): JsonObject? = runCatching {
        checkJsonEnvelope(text, MaxRecordBytes, 16)
        sanitize(Json.parseToJsonElement(text) as JsonObject)
    }.getOrNull()
}

/** A single bounded worker owns report writes. No executor or path lookup exists on the OFF path. */
internal object SelectedScanReports {
    private val sequence = AtomicLong()
    private val dropped = AtomicLong()
    private val executor by lazy {
        ThreadPoolExecutor(1, 1, 0, TimeUnit.MILLISECONDS, ArrayBlockingQueue(8), { work ->
            Thread(work, "PineSelectedScanReport").apply { isDaemon = true }
        }, ThreadPoolExecutor.AbortPolicy())
    }
    fun submit(context: Context, session: SelectedScanSession, fields: JsonObject) {
        if (!Diagnostics.selectedScanSessionMatches(session)) return
        val boundedFields = JsonObject(fields + ("dropped_report_count" to JsonPrimitive(dropped.get())))
        val record = runCatching { SelectedScanRecord.create(session, sequence.incrementAndGet(), boundedFields) }.getOrNull()
            ?: run { dropped.updateAndGet { if (it < Long.MAX_VALUE) it + 1 else it }; return }
        runCatching {
            executor.execute {
                if (!Diagnostics.selectedScanSessionMatches(session)) return@execute
                val persisted = runCatching { SelectedScanLogWriter(Diagnostics.directory(context)).append(record) }.getOrDefault(false)
                if (!persisted) dropped.updateAndGet { if (it < Long.MAX_VALUE) it + 1 else it }
                // Bounded live correlation uses the sink's own sequence. The
                // complete safe report remains in its independent fixed files.
                Diagnostics.fieldsForSession(session.id, "SELECTED_SCAN_FINISHED") {
                    val safeFields = record.getValue("fields").jsonObject
                    val native = safeFields["native_report"] as? JsonObject
                    val summary = native?.get("catalog_summary") as? JsonObject
                    val issue = (summary?.get("issue_codes") as? JsonArray)?.firstOrNull()
                        ?: native?.get("error_code")
                    listOf("scan_attempt_id", "profile", "terminal_status", "ui_resource_name", "ui_stage", "ui_outcome", "first_rejection_predicate")
                        .associateWith { ((safeFields[it] as? JsonPrimitive)?.contentOrNull ?: "none") } +
                        mapOf("first_native_issue" to ((issue as? JsonPrimitive)?.contentOrNull ?: "none"),
                            "report_persisted" to persisted.toString(), "dropped_report_count" to dropped.get().toString())
                }
            }
        }.onFailure { dropped.updateAndGet { if (it < Long.MAX_VALUE) it + 1 else it } }
    }
    internal fun awaitIdle(): Boolean {
        if (!executor.isShutdown) {
            val done = CountDownLatch(1)
            return runCatching { executor.execute { done.countDown() }; done.await(5, TimeUnit.SECONDS) }.getOrDefault(false)
        }
        return false
    }
}

/** Only two direct regular files, bounded rows/bytes. Never follows or replaces unsafe entries. */
internal class SelectedScanLogWriter(private val directory: File) {
    fun append(record: JsonObject): Boolean {
        val safe = SelectedScanRecord.sanitize(record) ?: return false
        val bytes = (safe.toString() + "\n").toByteArray(Charsets.UTF_8)
        if (bytes.size > SelectedScanRecord.MaxRecordBytes) return false
        ensureDirectory(directory.parentFile!!); ensureDirectory(directory)
        val current = File(directory, "selected-scan-0.jsonl")
        val previous = File(directory, "selected-scan-1.jsonl")
        for (file in listOf(current, previous)) if (Files.exists(file.toPath(), LinkOption.NOFOLLOW_LINKS) && !safeRegular(file)) return false
        if (current.exists() && (current.length() + bytes.size > SelectedScanRecord.MaxFileBytes || lineCount(current) >= SelectedScanRecord.MaxRecords)) {
            Files.deleteIfExists(previous.toPath())
            Files.move(current.toPath(), previous.toPath(), StandardCopyOption.REPLACE_EXISTING)
        }
        FileChannel.open(current.toPath(), StandardOpenOption.WRITE, StandardOpenOption.CREATE, StandardOpenOption.APPEND, LinkOption.NOFOLLOW_LINKS).use { channel ->
            require(channel.size() + bytes.size <= SelectedScanRecord.MaxFileBytes)
            val data = ByteBuffer.wrap(bytes)
            while (data.hasRemaining()) require(channel.write(data) > 0)
        }
        return true
    }
    private fun ensureDirectory(file: File) {
        require(file.absoluteFile == file.canonicalFile)
        if (!file.exists()) Files.createDirectory(file.toPath())
        require(Files.isDirectory(file.toPath(), LinkOption.NOFOLLOW_LINKS))
    }
    private fun safeRegular(file: File) = file.absoluteFile == file.canonicalFile &&
        Files.isRegularFile(file.toPath(), LinkOption.NOFOLLOW_LINKS) && file.length() <= SelectedScanRecord.MaxFileBytes
    private fun lineCount(file: File): Int = FileChannel.open(file.toPath(), StandardOpenOption.READ, LinkOption.NOFOLLOW_LINKS).use { channel ->
        val buffer = ByteBuffer.allocate(8192); var lines = 0; var budget = SelectedScanRecord.MaxFileBytes
        while (budget > 0 && lines < SelectedScanRecord.MaxRecords) {
            buffer.clear(); buffer.limit(minOf(buffer.capacity(), budget))
            val count = channel.read(buffer); if (count <= 0) break
            budget -= count; buffer.flip()
            while (buffer.hasRemaining()) if (buffer.get() == '\n'.code.toByte()) lines++
        }
        lines
    }
}

private fun JsonObject.recordText(key: String): String = (getValue(key) as JsonPrimitive).also { require(it.isString) }.content
private fun JsonObject.recordLong(key: String): Long = (getValue(key) as JsonPrimitive).also { require(!it.isString) }.longOrNull?.also { require(it >= 0) } ?: error("record_integer")
private fun JsonObject.recordCount(key: String) = recordLong(key).also { require(it <= Int.MAX_VALUE) }
private fun JsonElement?.safeNullable(allowed: Set<String>, unknown: String): JsonElement =
    if (this == JsonNull) JsonNull else JsonPrimitive((this as? JsonPrimitive)?.takeIf { it.isString }?.content.let { if (it in allowed) it!! else unknown })
