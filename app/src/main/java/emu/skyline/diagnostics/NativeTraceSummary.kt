// SPDX-License-Identifier: MPL-2.0
package emu.skyline.diagnostics

import java.io.ByteArrayOutputStream
import java.io.InputStream
import java.nio.ByteBuffer
import java.nio.charset.CodingErrorAction
import java.util.Locale
import java.util.concurrent.TimeUnit

/**
 * Narrow decoder for AOSP debuggerd/proto/tombstone.proto (blob 9deeeec9e185f79747acf5fb6a7e71586eb7da16).
 * Fixed message nesting only; unknown fields are skipped as bytes, never rendered or recursively expanded.
 * Never persists raw input. This is not a general protobuf decoder or proof of the memory writer.
 */
internal object NativeTraceSummary {
    const val MAX_BYTES = 256 * 1024
    private const val MAX_FRAMES = 16
    private val modules = setOf("libskyline.so", "libc.so", "libart.so", "libartd.so", "libutils.so", "libhwui.so",
        "libandroid_runtime.so", "liblog.so", "libc++_shared.so", "libnativebridge.so", "libndk_translation.so",
        "libunwindstack.so", "libbase.so", "linker64", "libm.so", "libdl.so", "libvulkan.so")
    data class Frame(val module: String, val relativePc: String, val buildId: String) {
        fun safeText() = "$module+$relativePc@${buildId.ifEmpty { "unknown_build_id" }}"
    }
    data class Result(val status: String, val signal: Int = 0, val abortSignature: String = "unknown",
        val frames: List<Frame> = emptyList(), val framesSeen: Int = 0, val framesOmitted: Int = 0) {
        fun fields(): Map<String, String> = mapOf("trace_status" to status, "signal_number" to signal.toString(),
            "abort_signature" to abortSignature, "frames" to frames.joinToString(";") { it.safeText() },
            "frames_seen" to framesSeen.toString(), "frames_omitted" to framesOmitted.toString(),
            "raw_trace_exported" to "false")
    }
    fun read(input: InputStream?, expectedPid: Int, expectedUid: Int, durationMs: Long = 1000): Result {
        if (input == null) return Result("null_or_overwritten")
        val deadline = System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(durationMs)
        return input.use { stream ->
            val out = ByteArrayOutputStream()
            val buffer = ByteArray(4096)
            var zeros = 0
            while (true) {
                if (System.nanoTime() >= deadline) return@use Result("read_deadline")
                val count = stream.read(buffer, 0, minOf(buffer.size, MAX_BYTES + 1 - out.size()))
                if (count < 0) break
                if (count == 0) { if (++zeros > 8) return@use Result("read_failed"); continue }
                out.write(buffer, 0, count)
                if (out.size() > MAX_BYTES) return@use Result("over_byte_limit")
            }
            parse(out.toByteArray(), expectedPid, expectedUid)
        }
    }
    private class Invalid : Exception()
    private class Budget(var remaining: Int = 32768) { fun spend() { if (--remaining < 0) throw Invalid() } }
    private data class Slice(val start: Int, val end: Int)
    private class Wire(val bytes: ByteArray, val start: Int, val end: Int, val budget: Budget) {
        var position = start
        fun number(): Long {
            budget.spend()
            var result = 0L
            for (index in 0..9) {
                if (position >= end) throw Invalid()
                val byte = bytes[position++].toInt() and 255
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
        fun slice(type: Int): Slice {
            if (type != 2) throw Invalid()
            val length = number(); if (length < 0 || length > end - position) throw Invalid()
            val result = Slice(position, position + length.toInt()); position = result.end
            return result
        }
        fun message(type: Int) = slice(type).let { Wire(bytes, it.start, it.end, budget) }
        fun string(type: Int, limit: Int): String {
            val value = slice(type)
            if (value.end - value.start > limit) return ""
            return Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT).onUnmappableCharacter(CodingErrorAction.REPORT)
                .decode(ByteBuffer.wrap(bytes, value.start, value.end - value.start)).toString()
        }
        fun skip(type: Int) {
            when (type) {
                0 -> number()
                1 -> { if (end - position < 8) throw Invalid(); position += 8 }
                2 -> slice(type)
                5 -> { if (end - position < 4) throw Invalid(); position += 4 }
                else -> throw Invalid() // Deprecated protobuf groups are not part of this schema.
            }
        }
        fun fields(operation: (Int, Int) -> Unit) {
            while (position < end) {
                val tag = number()
                if (tag <= 0 || tag ushr 3 > 0x1fffffff) throw Invalid()
                operation((tag ushr 3).toInt(), (tag and 7).toInt())
            }
        }
    }
    fun parse(bytes: ByteArray, expectedPid: Int, expectedUid: Int): Result {
        if (bytes.size > MAX_BYTES) return Result("over_byte_limit")
        return try {
            val budget = Budget()
            val top = Wire(bytes, 0, bytes.size, budget)
            var pid = -1; var uid = -1; var tid = -1; var signal = 0; var abort = "unknown"
            val threads = linkedMapOf<Int, Slice>()
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
                        if (threads.size >= 256) throw Invalid()
                        val entry = top.message(type); var key = -1; var value: Slice? = null
                        entry.fields { item, wire -> when (item) {
                            1 -> { if (key != -1) throw Invalid(); key = entry.integer(wire) }
                            2 -> { if (value != null) throw Invalid(); value = entry.slice(wire) }
                            else -> entry.skip(wire)
                        } }
                        if (key >= 0 && value != null) { if (threads.put(key, value!!) != null) throw Invalid() }
                    }
                    else -> top.skip(type) // fingerprint, command line, registers, memory/maps/logs/FDs are never rendered.
                }
            }
            if (pid != expectedPid || uid != expectedUid || expectedPid <= 0 || expectedUid <= 0) return Result("ownership_mismatch")
            val location = threads[tid] ?: return Result("fault_thread_unavailable", signal, abort)
            val thread = Wire(bytes, location.start, location.end, budget)
            val frames = mutableListOf<Frame>(); var seen = 0; var omitted = 0; var outputBytes = 0
            var threadId = -1
            thread.fields { item, type -> when (item) {
                1 -> { if (threadId != -1) throw Invalid(); threadId = thread.integer(type) }
                4 -> {
                    seen++
                    if (seen > 256) { thread.skip(type); omitted++ }
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
                        if (module !in modules || pc < 0 || pc > 0x0000ffffffffffffL || frames.size == MAX_FRAMES || outputBytes + size > 1700) omitted++
                        else { frames.add(safe); outputBytes += size }
                    }
                }
                else -> thread.skip(type)
            } }
            if (threadId != -1 && threadId != tid) return Result("ownership_mismatch")
            Result(if (frames.isEmpty()) "no_allowlisted_frames" else "ok", signal, abort, frames, seen, omitted)
        } catch (_: Exception) { Result("malformed_or_unsupported") }
    }
}
