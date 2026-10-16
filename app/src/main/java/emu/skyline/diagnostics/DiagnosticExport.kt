// SPDX-License-Identifier: MPL-2.0
package emu.skyline.diagnostics

import kotlinx.serialization.json.*
import java.io.ByteArrayOutputStream
import java.io.Closeable
import java.io.File
import java.io.OutputStream
import java.nio.channels.Channels
import java.nio.channels.FileChannel
import java.nio.file.LinkOption
import java.nio.file.StandardOpenOption
import java.util.UUID
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

/** Only fixed log names in the two known process directories (and the v1 legacy directory). */
object DiagnosticExport {
    const val MAX_FILE_BYTES = 8 * 1024 * 1024
    const val MAX_SNAPSHOT_BYTES = 40 * 1024 * 1024
    const val MAX_METADATA_BYTES = 1024 * 1024
    private const val MAX_RECORD_BYTES = 64 * 1024
    private const val MAX_IDENTITIES = 65536
    private val json = Json { isLenient = false }
    private val uri = Regex("(?i)(?:[a-z][a-z0-9+.-]*://)[^\\s\"<>]+")
    private val privatePath = Regex("(?i)(?:/(?:storage|sdcard|data|mnt|home|Users)(?:/[^\\s\"<>]*)?|[A-Z]:[\\\\/][^\\s\"<>]+)")
    private val sensitiveName = Regex("(?i)(?:password|passwd|(?:access[_-]?|refresh[_-]?|auth[_-]?)?token|secret|credential|ticket|authorization|api[_-]?key|private[_-]?key|cookie)")
    private val credentialLine = Regex("(?i)(?:prod\\.keys|title\\.keys|\\b(?:password|passwd|(?:access[_-]?|refresh[_-]?|auth[_-]?)?token|secret|credential|ticket|authorization|api[_-]?key|private[_-]?key|cookie)\\b[\\s=:])")
    private val snapshotId = Regex("snapshot-[0-9a-f-]{36}")
    private val names = (listOf("emulation/", "main/", "").flatMap { scope ->
        (0..7).map { "${scope}diagnostics-$it.jsonl" }
    } + "emulation.log")

    /** For plain text and individual JSON string values; never run a regexp across JSON syntax. */
    fun redact(text: String): String = text.lineSequence().joinToString("\n") { line ->
        if (credentialLine.containsMatchIn(line)) "[REDACTED_CREDENTIAL_LINE]"
        else privatePath.replace(uri.replace(line, "[REDACTED_URI]"), "[REDACTED_PATH]")
    }

    fun redactJson(record: JsonObject): JsonObject = redactElement(record) as JsonObject

    private fun redactElement(value: JsonElement): JsonElement = when (value) {
        is JsonObject -> JsonObject(value.entries.associate { (key, child) ->
            redact(key) to if (sensitiveName.containsMatchIn(key)) JsonPrimitive("[REDACTED_SECRET]") else redactElement(child)
        })
        is JsonArray -> JsonArray(value.map(::redactElement))
        is JsonPrimitive -> if (value.isString) JsonPrimitive(redact(value.content)) else value
    }

    data class Entry(val name: String, val bytes: Long)

    /** Redacted copies are kept in private cache, so the preview and ZIP contain the same bytes. */
    class Snapshot internal constructor(private val directory: File) : Closeable {
        val id: String get() = directory.name
        val entries: List<Entry> get() = archiveNames.mapNotNull { name ->
            val file = File(directory, name)
            val limit = if (name == "session-metadata.txt") MAX_METADATA_BYTES else MAX_FILE_BYTES
            if (isSafeFile(file) && file.length() <= limit) Entry(name, file.length()) else null
        }
        internal fun file(name: String) = File(directory, name)
        override fun close() {
            // Delete only our allowlisted copies, even when restoring after activity recreation.
            // A symlink at a scope directory must be unlinked, never traversed to its children.
            fun deleteEntry(file: File) {
                val safeParent = runCatching {
                    val parent = file.parentFile!!
                    parent.absoluteFile == parent.canonicalFile
                }.getOrDefault(false)
                if (safeParent) file.delete()
            }
            archiveNames.forEach { deleteEntry(File(directory, it)) }
            deleteEntry(File(directory, "main")); deleteEntry(File(directory, "emulation"))
            deleteEntry(directory)
        }
    }

    private val archiveNames = listOf("session-metadata.txt") + names

    private fun isSafeFile(file: File): Boolean = runCatching {
        file.absoluteFile == file.canonicalFile &&
            java.nio.file.Files.isRegularFile(file.toPath(), LinkOption.NOFOLLOW_LINKS)
    }.getOrDefault(false)

    private fun sources(directory: File, textLog: File) = names.map { name ->
        name to if (name == "emulation.log") textLog else File(directory, name)
    }

    fun files(directory: File, textLog: File): List<File> = sources(directory, textLog)
        .map { it.second }.filter { isSafeFile(it) && it.length() <= MAX_FILE_BYTES }

    fun restore(cacheDirectory: File, id: String): Snapshot? {
        if (!snapshotId.matches(id)) return null
        val directory = File(cacheDirectory, id)
        return if (directory.absoluteFile == directory.canonicalFile && directory.isDirectory &&
            isSafeFile(File(directory, "session-metadata.txt")) && File(directory, "session-metadata.txt").length() <= MAX_METADATA_BYTES) Snapshot(directory) else null
    }

    /**
     * Read from opened descriptors, bounded to their initial lengths. Native rotation renames files;
     * an opened descriptor continues to refer to the same file. Complete JSON records are deduplicated
     * by session/sequence when a rotation exposes a record under two names. This is a best-effort
     * live snapshot, not a native queue flush or an atomic snapshot of all process logs.
     */
    fun capture(directory: File, textLog: File, cacheDirectory: File, metadata: String): Snapshot =
        captureBounded(directory, textLog, cacheDirectory, metadata, MAX_SNAPSHOT_BYTES)

    @Synchronized internal fun captureBounded(directory: File, textLog: File, cacheDirectory: File, metadata: String, byteBudget: Int): Snapshot {
        require(byteBudget in 1..MAX_SNAPSHOT_BYTES)
        require(cacheDirectory.absoluteFile == cacheDirectory.canonicalFile)
        check(cacheDirectory.isDirectory || cacheDirectory.mkdirs())
        cacheDirectory.listFiles()?.filter { snapshotId.matches(it.name) }
            ?.sortedByDescending { it.lastModified() }?.forEachIndexed { index, file ->
                if (index >= 1 || file.lastModified() < System.currentTimeMillis() - 24 * 60 * 60 * 1000L) {
                    // A killed process can leave an incomplete snapshot without metadata.
                    // Still remove only fixed allowlisted copies in a direct, non-symlink child.
                    if (file.absoluteFile == file.canonicalFile &&
                        java.nio.file.Files.isDirectory(file.toPath(), LinkOption.NOFOLLOW_LINKS)) Snapshot(file).close()
                }
            }
        val target = File(cacheDirectory, "snapshot-${UUID.randomUUID()}")
        check(target.mkdir())
        val snapshot = Snapshot(target)
        val summary = Summary(byteBudget.toLong())
        try {
            for ((name, source) in sources(directory, textLog)) {
                if (!source.exists()) continue
                if (summary.remainingBytes == 0L) {
                    summary.omissions.add("$name: snapshot byte limit reached")
                    continue
                }
                if (!isSafeFile(source)) { summary.omissions.add("$name: not a regular private log"); continue }
                val channel = try {
                    FileChannel.open(source.toPath(), StandardOpenOption.READ, LinkOption.NOFOLLOW_LINKS)
                } catch (_: java.io.IOException) {
                    summary.omissions.add("$name: unavailable during snapshot")
                    continue
                }
                channel.use {
                    val length = channel.size()
                    if (length > MAX_FILE_BYTES) {
                        summary.omissions.add("$name: exceeds $MAX_FILE_BYTES bytes")
                    } else {
                        val destination = snapshot.file(name)
                        check(destination.parentFile!!.isDirectory || destination.parentFile!!.mkdirs())
                        destination.outputStream().buffered().use { output ->
                            copy(channel, length, output, name.endsWith(".jsonl"), summary)
                        }
                    }
                }
            }
            val metadataBytes = (redact(metadata.take(16384)) + "\nmetadata_header_truncated=${metadata.length > 16384}\n" + summary.render()).toByteArray(Charsets.UTF_8)
            check(metadataBytes.size <= MAX_METADATA_BYTES) { "Export metadata limit exceeded" }
            snapshot.file("session-metadata.txt").writeBytes(metadataBytes)
            return snapshot
        } catch (error: Throwable) {
            snapshot.close()
            throw error
        }
    }

    private fun copy(channel: FileChannel, length: Long, output: OutputStream, structured: Boolean, summary: Summary) {
        val line = ByteArrayOutputStream()
        var oversized = false
        var remaining = length
        var written = 0L
        val chunk = ByteArray(8192)
        val input = Channels.newInputStream(channel)
        fun writeLine(text: String, newline: Boolean) {
            val bytes = text.toByteArray(Charsets.UTF_8)
            val size = bytes.size + if (newline) 1 else 0
            if (size + written > MAX_FILE_BYTES) {
                summary.exportLimit++; summary.fileLimit++
            } else if (size > summary.remainingBytes) {
                summary.exportLimit++; summary.snapshotLimit++
            } else {
                output.write(bytes)
                if (newline) output.write('\n'.code)
                written += size
                summary.remainingBytes -= size
            }
        }
        fun finishLine(complete: Boolean) {
            if (oversized) summary.oversized++
            else if (structured) {
                if (!complete) summary.incomplete++
                else if (line.size() != 0) {
                    val record = runCatching { json.parseToJsonElement(line.toString("UTF-8")) as? JsonObject }.getOrNull()
                    if (record == null) summary.malformed++
                    else if (summary.accept(record)) {
                        val safe = redactJson(record)
                        val before = written
                        writeLine(safe.toString(), true)
                        if (written != before) summary.observe(safe)
                    }
                }
            } else {
                writeLine(redact(line.toString("UTF-8")), complete)
            }
            line.reset(); oversized = false
        }
        while (remaining > 0) {
            val count = input.read(chunk, 0, minOf(chunk.size.toLong(), remaining).toInt())
            if (count <= 0) break
            remaining -= count
            for (index in 0 until count) {
                val byte = chunk[index]
                if (byte == '\n'.code.toByte()) finishLine(true)
                else if (line.size() < MAX_RECORD_BYTES) line.write(byte.toInt())
                else oversized = true
            }
        }
        if (line.size() != 0 || oversized) finishLine(false)
        if (remaining != 0L) summary.truncated++
    }

    private class Summary(private val byteBudget: Long) {
        var remainingBytes = byteBudget
        var records = 0L
        var malformed = 0L
        var incomplete = 0L
        var oversized = 0L
        var duplicates = 0L
        var truncated = 0L
        var exportLimit = 0L
        var fileLimit = 0L
        var snapshotLimit = 0L
        val omissions = mutableListOf<String>()
        private val identities = HashSet<Pair<String, Long>>()
        private var dedupLimit = false
        private val sessions = sortedSetOf<String>()
        private val sessionBuilds = linkedMapOf<String, String>()
        private val content = ArrayDeque<String>()
        private val errors = ArrayDeque<String>()
        private var errorCount = 0L
        private val stages = linkedMapOf<String, Pair<Long, String>>()
        fun accept(record: JsonObject): Boolean {
            val session = (record["session_id"] as? JsonPrimitive)?.contentOrNull
            val sequence = (record["sequence"] as? JsonPrimitive)?.longOrNull
            if (session != null && session.length <= 128 && sequence != null) {
                val identity = session to sequence
                if (identity in identities) { duplicates++; return false }
            }
            return true
        }
        fun observe(record: JsonObject) {
            records++
            val session = (record["session_id"] as? JsonPrimitive)?.contentOrNull.orEmpty().take(128)
            if (session.isNotEmpty() && sessions.size < 64) sessions.add(session)
            val category = (record["category"] as? JsonPrimitive)?.contentOrNull
            val event = (record["event"] as? JsonPrimitive)?.contentOrNull.orEmpty().take(96)
            val level = (record["level"] as? JsonPrimitive)?.contentOrNull
            val stage = (record["last_stage"] as? JsonPrimitive)?.contentOrNull
            val sequence = (record["sequence"] as? JsonPrimitive)?.longOrNull ?: -1L
            if (session.isNotEmpty() && sequence >= 0) {
                if (identities.size < MAX_IDENTITIES) identities.add(session to sequence) else dedupLimit = true
            }
            if (!stage.isNullOrEmpty() && (session in stages || stages.size < 64) && sequence >= (stages[session]?.first ?: -1L)) {
                stages[session] = sequence to stage.take(128)
            }
            val fullRow = buildJsonObject {
                listOf("session_id", "sequence", "wall_clock_ms", "title_id", "event", "level", "fields").forEach { key ->
                    record[key]?.let { put(key, it) }
                }
            }.toString()
            val row = if (fullRow.length <= 1024) fullRow else buildJsonObject {
                put("event", event); put("session_id", session)
                put("summary_truncated", true); put("excerpt", fullRow.take(512))
            }.toString()
            if (event == "SESSION_METADATA" && (session in sessionBuilds || sessionBuilds.size < 64)) sessionBuilds[session] = row
            if (category == "content") {
                if (content.size == 64) content.removeFirst()
                content.addLast(row)
            }
            if (level in listOf("ERROR", "CRITICAL", "FATAL") || event.contains("EXCEPTION") || event.endsWith("_ERROR")) {
                errorCount++
                if (errors.size == 32) errors.removeFirst()
                errors.addLast(row)
            }
        }
        fun render() = buildString {
            appendLine("snapshot=bounded live snapshot; files may span sessions; queued events are not flushed")
            appendLine("snapshot_log_byte_budget=$byteBudget")
            appendLine("snapshot_exported_log_bytes=${byteBudget - remainingBytes}")
            appendLine("snapshot_priority=emulation, main, legacy, emulation.log")
            appendLine("snapshot_rotation=opened file descriptors; duplicate session/sequence records omitted; concurrent rotation may omit records")
            appendLine("exported_records=$records")
            appendLine("observed_sessions=${sessions.joinToString(",")}")
            appendLine("observed_session_build_metadata=SESSION_METADATA emitted by each captured session; current exporter build is separate")
            sessionBuilds.values.forEach(::appendLine)
            appendLine("observed_sessions_without_build_metadata=${sessions.filterNot { it in sessionBuilds }.joinToString(",")}")
            appendLine("omitted_malformed_records=$malformed")
            appendLine("omitted_incomplete_records=$incomplete")
            appendLine("omitted_oversized_lines=$oversized")
            appendLine("omitted_duplicate_records=$duplicates")
            appendLine("deduplication_limit_reached=$dedupLimit")
            appendLine("files_shortened_while_reading=$truncated")
            appendLine("omitted_lines_at_export_byte_limit=$exportLimit")
            appendLine("omitted_lines_at_file_byte_limit=$fileLimit")
            appendLine("omitted_lines_at_snapshot_byte_limit=$snapshotLimit")
            omissions.forEach { appendLine("omitted_file=$it") }
            appendLine("last_observed_stages=${stages.entries.joinToString { "${it.key}:${it.value.second}" }}")
            appendLine("observed_content_summary=up to 64 most recently captured content events, in snapshot read order")
            content.forEach(::appendLine)
            appendLine("observed_error_count=$errorCount")
            appendLine("observed_error_summary=up to 32 most recently captured errors, in snapshot read order")
            errors.forEach(::appendLine)
            appendLine("Missing events do not prove that a path was not executed. Guest text may contain personal information.")
        }
    }

    fun write(output: OutputStream, snapshot: Snapshot) {
        val entries = snapshot.entries
        require(entries.any { it.name == "session-metadata.txt" }) { "Export snapshot expired" }
        require(entries.filter { it.name != "session-metadata.txt" }.sumOf { it.bytes } <= MAX_SNAPSHOT_BYTES)
        val channels = mutableListOf<Pair<Entry, FileChannel>>()
        try {
            for (entry in entries) {
                val source = snapshot.file(entry.name)
                require(isSafeFile(source))
                val channel = FileChannel.open(source.toPath(), StandardOpenOption.READ, LinkOption.NOFOLLOW_LINKS)
                channels.add(entry to channel)
                require(channel.size() == entry.bytes && entry.bytes <= MAX_FILE_BYTES)
            }
            ZipOutputStream(output).use { zip ->
                val buffer = ByteArray(8192)
                for ((entry, channel) in channels) {
                    zip.putNextEntry(ZipEntry(entry.name))
                    val input = Channels.newInputStream(channel)
                    var remaining = entry.bytes
                    while (remaining > 0) {
                        val read = input.read(buffer, 0, minOf(buffer.size.toLong(), remaining).toInt())
                        check(read > 0) { "Export snapshot shortened" }
                        zip.write(buffer, 0, read); remaining -= read
                    }
                    zip.closeEntry()
                }
            }
        } finally {
            channels.forEach { it.second.close() }
        }
    }
}
