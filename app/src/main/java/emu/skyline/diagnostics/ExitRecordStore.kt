// SPDX-License-Identifier: MPL-2.0
package emu.skyline.diagnostics

import java.io.File
import java.nio.ByteBuffer
import java.nio.channels.FileChannel
import java.nio.file.LinkOption
import java.nio.file.StandardOpenOption
import java.security.MessageDigest

internal object ExitRecordIdentity {
    fun id(timestamp: Long, reason: Int, status: Int, pid: Int, scope: String): String {
        val bytes = ByteBuffer.allocate(25).putLong(timestamp).putInt(reason).putInt(status).putInt(pid)
            .put(if (scope == "emulation") 1.toByte() else 0.toByte()).putInt(1).array()
        return MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it.toInt() and 255) }
    }
}
/** Observer threads only. A cross-process try-lock prevents simultaneous history re-imports. */
internal class ExitRecordStore(private val directory: File) {
    private val pattern = Regex("[et]:[0-9a-f]{64}")
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
