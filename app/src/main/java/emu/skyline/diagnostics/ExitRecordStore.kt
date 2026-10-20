// SPDX-License-Identifier: MPL-2.0
package emu.skyline.diagnostics

import java.io.File
import java.nio.ByteBuffer
import java.nio.channels.FileChannel
import java.nio.file.LinkOption
import java.nio.file.StandardOpenOption
import java.security.MessageDigest

internal object ExitRecordIdentity {
    // Android may refine status (e.g. 0 -> SIGABRT) after the same exit is first queried.
    // It is an observation, not part of stable exit identity.
    @Suppress("UNUSED_PARAMETER")
    fun id(timestamp: Long, reason: Int, status: Int, pid: Int, scope: String): String =
        digest(timestamp, reason, 0, pid, scope, 2)
    fun legacyId(timestamp: Long, reason: Int, status: Int, pid: Int, scope: String): String =
        digest(timestamp, reason, status, pid, scope, 1)
    private fun digest(timestamp: Long, reason: Int, status: Int, pid: Int, scope: String, version: Int): String {
        val bytes = ByteBuffer.allocate(25).putLong(timestamp).putInt(reason).putInt(status).putInt(pid)
            .put(if (scope == "emulation") 1.toByte() else 0.toByte()).putInt(version).array()
        return MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it.toInt() and 255) }
    }

    fun previousStatus(seen: Set<String>, id: String): Int? = seen.lastOrNull { it.startsWith("v2:$id:") }
        ?.substringAfterLast(':')?.toIntOrNull()
    private fun pendingMigrationStatus(seen: Set<String>, id: String): Int? = seen.lastOrNull { it.startsWith("m2:$id:") }
        ?.substringAfterLast(':')?.toIntOrNull()
    fun needsObservation(seen: Set<String>, id: String, status: Int): Boolean =
        "e:$id" !in seen || previousStatus(seen, id) != status || pendingMigrationStatus(seen, id) != null
    fun observationFields(seen: Set<String>, id: String, timestamp: Long, reason: Int, status: Int, pid: Int, scope: String): Map<String, String> {
        val migration = pendingMigrationStatus(seen, id)
        // Keep the original migration alias even if Android refines status again
        // before publication succeeds. At most two legacy IDs are exported.
        val legacyStatus = migration ?: status
        return mapOf("exit_record_id" to id, "previous_status" to (previousStatus(seen, id)?.toString() ?: "unknown"),
            "legacy_exit_record_id" to legacyId(timestamp, reason, legacyStatus, pid, scope),
            "legacy_initial_exit_record_id" to legacyId(timestamp, reason, 0, pid, scope),
            "legacy_exit_status" to legacyStatus.toString()) +
            if (migration != null) mapOf("migration_reason" to "legacy_status_identity_v1_to_v2") else emptyMap()
    }
    // Call only after the event has been accepted and flushed.
    fun markObserved(seen: MutableSet<String>, id: String, status: Int) {
        seen.add("e:$id")
        seen.removeAll { it.startsWith("v2:$id:") || it.startsWith("m2:$id:") }
        seen.add("v2:$id:$status")
    }
    /** Migrate the known legacy initial(0)/current status aliases only. New events
     * carry both aliases so already-exported evidence can be correlated as well.
     */
    fun migrate(seen: MutableSet<String>, timestamp: Long, reason: Int, status: Int, pid: Int, scope: String): String {
        val id = id(timestamp, reason, status, pid, scope)
        val aliases = listOf(0, status).distinct().map { it to legacyId(timestamp, reason, it, pid, scope) }
        val observed = aliases.lastOrNull { "e:${it.second}" in seen }
        if ("e:$id" !in seen && observed != null) {
            seen.add("e:$id")
            seen.add("v2:$id:${observed.first}")
        }
        // Persist a pending bridge before deleting legacy markers. A failed
        // admission/flush must not lose the only link to already-exported rows.
        if (observed != null && pendingMigrationStatus(seen, id) == null) seen.add("m2:$id:$status")
        val oldAttempts = aliases.sumOf { (_, old) -> (1..NativeTraceImport.MAX_ATTEMPTS).lastOrNull { "r1:$old:$it" in seen } ?: 0 }
        val currentAttempts = (1..NativeTraceImport.MAX_ATTEMPTS).lastOrNull { "r1:$id:$it" in seen } ?: 0
        val attempts = (oldAttempts + currentAttempts).coerceAtMost(NativeTraceImport.MAX_ATTEMPTS)
        if (attempts > 0) {
            (1..NativeTraceImport.MAX_ATTEMPTS).forEach { seen.remove("r1:$id:$it") }
            seen.add("r1:$id:$attempts")
        }
        for ((_, old) in aliases) {
            if ("s1:$old" in seen) seen.add("s1:$id")
            if ("p1:$old" in seen) seen.add("p1:$id")
            seen.removeAll { it == "e:$old" || it == "t:$old" || it == "s1:$old" || it == "p1:$old" || it.startsWith("r1:$old:") }
        }
        return id
    }
}
/** Observer threads only. A cross-process try-lock prevents simultaneous history re-imports. */
internal class ExitRecordStore(private val directory: File) {
    private val pattern = Regex("(?:[et]:[0-9a-f]{64}|[sp]1:[0-9a-f]{64}|[vm]2:[0-9a-f]{64}:-?[0-9]{1,10}|r1:[0-9a-f]{64}:[1-3])")
    fun <T> locked(operation: (MutableSet<String>) -> T): T {
        require(directory.absoluteFile == directory.canonicalFile)
        check(directory.isDirectory || directory.mkdirs())
        val options = arrayOf<java.nio.file.OpenOption>(StandardOpenOption.CREATE, StandardOpenOption.WRITE, LinkOption.NOFOLLOW_LINKS)
        FileChannel.open(File(directory, "exit-records.lock").toPath(), *options).use { channel ->
            val lock = channel.tryLock() ?: throw IllegalStateException("history_dedup_busy")
            lock.use {
                val file = File(directory, "exit-records.txt")
                val seen = linkedSetOf<String>()
                if (file.exists()) {
                    require(file.absoluteFile == file.canonicalFile && file.length() <= 16384)
                    FileChannel.open(file.toPath(), StandardOpenOption.READ, LinkOption.NOFOLLOW_LINKS).use { input ->
                        val data = ByteBuffer.allocate(16385)
                        while (data.hasRemaining() && input.read(data) > 0) { }
                        require(data.position() <= 16384)
                        String(data.array(), 0, data.position(), Charsets.US_ASCII).lineSequence().filter(pattern::matches).forEach { seen.add(it) }
                    }
                }
                val result = operation(seen)
                val text = seen.filter(pattern::matches).takeLast(128).joinToString("\n", postfix = "\n")
                val temporary = File(directory, "exit-records.tmp")
                FileChannel.open(temporary.toPath(), *options, StandardOpenOption.TRUNCATE_EXISTING).use { out ->
                    val buffer = ByteBuffer.wrap(text.toByteArray(Charsets.US_ASCII))
                    while (buffer.hasRemaining()) check(out.write(buffer) > 0)
                }
                check(temporary.renameTo(file))
                return result
            }
        }
    }
}
