// SPDX-License-Identifier: MPL-2.0
package emu.skyline.diagnostics

import java.io.InputStream
import java.io.IOException
import java.nio.ByteBuffer
import java.nio.charset.CodingErrorAction
import java.util.Locale
import java.util.concurrent.TimeUnit

/** Narrow streaming decoder for AOSP debuggerd/proto/tombstone.proto.
 * Unknown fields (including maps/registers/memory) are discarded in fixed chunks.
 * Only completed allowlisted summaries are retained; raw input is never persisted.
 */
internal object NativeTraceSummary {
    const val MAX_BYTES = 256 * 1024 // Historical whole-input limit; retained for regression fixtures.
    const val MAX_SCAN_BYTES = 8 * 1024 * 1024
    private const val MAX_FRAMES = 16
    private val modules = setOf("libskyline.so", "libc.so", "libart.so", "libartd.so", "libutils.so", "libhwui.so",
        "libandroid_runtime.so", "liblog.so", "libc++_shared.so", "libnativebridge.so", "libndk_translation.so",
        "libunwindstack.so", "libbase.so", "linker64", "libm.so", "libdl.so", "libvulkan.so")
    data class Frame(val module: String, val relativePc: String, val buildId: String) {
        fun safeText() = "$module+$relativePc@${buildId.ifEmpty { "unknown_build_id" }}"
    }
    data class Result(val status: String, val signal: Int = 0, val abortSignature: String = "unknown",
        val frames: List<Frame> = emptyList(), val framesSeen: Int = 0, val framesOmitted: Int = 0,
        val complete: Boolean = false, val bytesRead: Int = 0, val captureStatus: String = status) {
        fun fields(): Map<String, String> = mapOf("trace_status" to status, "signal_number" to signal.toString(),
            "abort_signature" to abortSignature, "frames" to frames.joinToString(";") { it.safeText() },
            "frames_seen" to framesSeen.toString(), "frames_omitted" to framesOmitted.toString(),
            "raw_trace_exported" to "false", "trace_complete" to complete.toString(),
            "trace_bytes_read" to bytesRead.toString(), "trace_capture_status" to captureStatus)
    }
    private class Invalid : Exception()
    private class Stopped(val reason: String) : Exception()
    private class Source(val input: InputStream, durationMs: Long) {
        private val deadline = System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(durationMs.coerceIn(0, 10_000))
        private val buffer = ByteArray(4096)
        private var cursor = 0
        private var available = 0
        var position = 0L; private set
        var bytesRead = 0; private set
        private var operations = 32768
        fun spend() {
            if (System.nanoTime() >= deadline || Thread.currentThread().isInterrupted) throw Stopped("truncated_deadline")
            if (--operations < 0) throw Stopped("truncated_work_limit")
        }
        fun peek(): Int? {
            if (cursor == available) {
                spend()
                if (bytesRead == MAX_SCAN_BYTES) throw Stopped("truncated_byte_limit")
                var zeroReads = 0
                while (true) {
                    val count = input.read(buffer, 0, minOf(buffer.size, MAX_SCAN_BYTES - bytesRead))
                    if (count < 0) return null
                    if (count > 0) { bytesRead += count; cursor = 0; available = count; break }
                    if (++zeroReads > 8) throw IOException("zero length reads")
                    spend()
                }
            }
            return buffer[cursor].toInt() and 255
        }
        fun next(): Int = (peek() ?: throw Stopped("truncated_input")).also { cursor++; position++ }
        fun discard(size: Long) {
            var remaining = size
            while (remaining > 0) {
                peek() ?: throw Stopped("truncated_input")
                val count = minOf(remaining, (available - cursor).toLong()).toInt()
                cursor += count; position += count; remaining -= count
            }
        }
    }
    private class Wire(val source: Source, val end: Long = Long.MAX_VALUE) {
        fun number(): Long {
            source.spend()
            var result = 0L
            for (index in 0..9) {
                if (source.position >= end) throw Invalid()
                val byte = source.next()
                if (index == 9 && byte > 1) throw Invalid()
                result = result or ((byte and 127).toLong() shl (index * 7))
                if (byte and 128 == 0) return result
            }
            throw Invalid()
        }
        fun integer(type: Int): Int {
            if (type != 0) throw Invalid()
            val value = number(); if (value < 0 || value > Int.MAX_VALUE) throw Invalid()
            return value.toInt()
        }
        private fun length(type: Int): Long {
            if (type != 2) throw Invalid()
            val size = number()
            if (size < 0 || size > end - source.position) throw Invalid()
            return size
        }
        fun message(type: Int): Wire {
            val size = length(type)
            return Wire(source, source.position + size)
        }
        fun string(type: Int, limit: Int): String {
            val size = length(type)
            if (size > limit) { source.discard(size); return "" }
            val bytes = ByteArray(size.toInt()) { source.next().toByte() }
            return Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT).onUnmappableCharacter(CodingErrorAction.REPORT)
                .decode(ByteBuffer.wrap(bytes)).toString()
        }
        fun skip(type: Int) {
            val size = when (type) {
                0 -> { number(); return }
                1 -> 8L
                2 -> length(type)
                5 -> 4L
                else -> throw Invalid()
            }
            if (size > end - source.position) throw Invalid()
            source.discard(size)
        }
        fun fields(operation: (Int, Int) -> Unit) {
            while (source.position < end) {
                if (source.peek() == null) {
                    if (end == Long.MAX_VALUE) return else throw Stopped("truncated_input")
                }
                val tag = number()
                if (tag < 8 || tag ushr 3 > 0x1fffffff) throw Invalid()
                operation((tag ushr 3).toInt(), (tag and 7).toInt())
            }
        }
    }
    private class ThreadSummary {
        var threadId = -1
        val frames = mutableListOf<Frame>()
        var seen = 0; var omitted = 0; var outputBytes = 0
        var pendingFrame = false
    }
    private class Entry { var key = -1; var thread: ThreadSummary? = null }
    private class Decoder(val source: Source, val expectedPid: Int, val expectedUid: Int) {
        var pid = -1; var uid = -1; var tid = -1; var signal = 0; var abort = "unknown"
        private val entries = mutableListOf<Entry>()
        private val keys = hashSetOf<Int>()
        private var retainedFrameBytes = 0
        fun decode() {
            val top = Wire(source)
            val unique = hashSetOf<Int>()
            top.fields { field, type ->
                if (field in setOf(5, 6, 7, 10, 14) && !unique.add(field)) throw Invalid()
                when (field) {
                    5 -> pid = top.integer(type)
                    6 -> tid = top.integer(type)
                    7 -> uid = top.integer(type)
                    10 -> {
                        val info = top.message(type); var found = false
                        info.fields { item, wire -> if (item == 1) { if (found) throw Invalid(); found = true; signal = info.integer(wire) } else info.skip(wire) }
                    }
                    14 -> {
                        val text = top.string(type, 8192)
                        abort = when {
                            text.contains("GC tried to mark invalid reference") -> "ART_GC_INVALID_REFERENCE"
                            text.contains("invalid pthread_t") -> "INVALID_PTHREAD_HANDLE"
                            else -> "unknown"
                        }
                    }
                    16 -> {
                        if (entries.size >= 256) throw Stopped("truncated_thread_limit")
                        val entry = Entry(); entries.add(entry)
                        val message = top.message(type)
                        message.fields { item, wire -> when (item) {
                            1 -> {
                                if (entry.key != -1) throw Invalid()
                                entry.key = message.integer(wire)
                                if (!keys.add(entry.key)) throw Invalid()
                            }
                            2 -> {
                                if (entry.thread != null) throw Invalid()
                                val summary = ThreadSummary(); entry.thread = summary
                                if (tid >= 0 && entry.key >= 0 && entry.key != tid) message.skip(wire)
                                else readThread(message.message(wire), summary)
                            }
                            else -> message.skip(wire)
                        } }
                    }
                    else -> top.skip(type)
                }
            }
        }
        private fun readThread(thread: Wire, summary: ThreadSummary) {
            thread.fields { item, type -> when (item) {
                1 -> { if (summary.threadId != -1) throw Invalid(); summary.threadId = thread.integer(type) }
                4 -> {
                    summary.seen++
                    summary.pendingFrame = true
                    if (summary.seen > 256) { thread.skip(type); summary.omitted++ }
                    else {
                        val frame = thread.message(type); var pc = 0L; var path = ""; var buildId = ""
                        val known = hashSetOf<Int>()
                        frame.fields { field, wire ->
                            if (field in setOf(1, 6, 8) && !known.add(field)) throw Invalid()
                            when (field) {
                                1 -> { if (wire != 0) throw Invalid(); pc = frame.number() }
                                6 -> path = frame.string(wire, 512)
                                8 -> buildId = frame.string(wire, 128)
                                else -> frame.skip(wire)
                            }
                        }
                        val module = path.substringAfterLast('/').substringAfterLast('!')
                        val safeId = buildId.takeIf { it.length in 8..64 && it.length % 2 == 0 && it.all { char -> char in "0123456789abcdefABCDEF" } }?.lowercase(Locale.ROOT).orEmpty()
                        val safe = Frame(module, "0x${pc.toString(16)}", safeId)
                        val size = safe.safeText().length + 1
                        if (1 !in known || module !in modules || pc < 0 || pc > 0x0000ffffffffffffL || summary.frames.size == MAX_FRAMES || summary.outputBytes + size > 1700 || retainedFrameBytes + size > 65536) summary.omitted++
                        else { summary.frames.add(safe); summary.outputBytes += size; retainedFrameBytes += size }
                    }
                    summary.pendingFrame = false
                }
                else -> thread.skip(type)
            } }
        }
        fun result(capture: String): Result {
            val complete = capture == "complete"
            if (expectedPid <= 0 || expectedUid <= 0 || (pid != -1 && pid != expectedPid) || (uid != -1 && uid != expectedUid))
                return Result("ownership_mismatch", bytesRead = source.bytesRead, captureStatus = capture)
            if (pid == -1 || uid == -1) return Result("ownership_unverified", bytesRead = source.bytesRead, captureStatus = capture)
            if (tid <= 0) return Result("ownership_unverified", bytesRead = source.bytesRead, captureStatus = capture)
            val summary = entries.firstOrNull { it.key == tid && it.key > 0 }?.thread
            if (summary != null && summary.threadId <= 0)
                return Result("ownership_unverified", bytesRead = source.bytesRead, captureStatus = capture)
            if (summary != null && summary.threadId != tid)
                return Result("ownership_mismatch", bytesRead = source.bytesRead, captureStatus = capture)
            val status = if (!complete) capture else if (summary == null) "fault_thread_unavailable" else if (summary.frames.isEmpty()) "no_allowlisted_frames" else "ok"
            return Result(status, signal, abort, summary?.frames?.toList().orEmpty(), summary?.seen ?: 0,
                (summary?.omitted ?: 0) + (if (summary?.pendingFrame == true) 1 else 0), complete, source.bytesRead, capture)
        }
    }
    fun read(input: InputStream?, expectedPid: Int, expectedUid: Int, durationMs: Long = 1000): Result {
        if (input == null) return Result("null_or_overwritten")
        val source = Source(input, durationMs)
        val decoder = Decoder(source, expectedPid, expectedUid)
        return try {
            decoder.decode(); decoder.result("complete")
        } catch (stop: Stopped) { decoder.result(stop.reason) }
        catch (_: java.nio.charset.CharacterCodingException) { Result("malformed_or_unsupported", bytesRead = source.bytesRead) }
        catch (_: IOException) { decoder.result("truncated_read_failed") }
        catch (_: Exception) { Result("malformed_or_unsupported", bytesRead = source.bytesRead) }
        finally {
            // A close failure must not discard an already captured safe summary.
            runCatching { input.close() }
        }
    }
    fun parse(bytes: ByteArray, expectedPid: Int, expectedUid: Int): Result = read(bytes.inputStream(), expectedPid, expectedUid)
}
