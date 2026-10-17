// SPDX-License-Identifier: MPL-2.0
package emu.skyline.diagnostics

import org.junit.Assert.*
import org.junit.Test
import java.io.ByteArrayOutputStream
import java.io.InputStream
import java.util.Random
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.TimeoutException

class NativeTraceSummaryTest {
    private fun varint(value: Long): ByteArray {
        val out = ByteArrayOutputStream(); var next = value
        while (next and -128L != 0L) { out.write(((next and 127) or 128).toInt()); next = next ushr 7 }
        out.write(next.toInt()); return out.toByteArray()
    }
    private fun scalar(field: Int, value: Long) = varint((field * 8).toLong()) + varint(value)
    private fun blob(field: Int, bytes: ByteArray) = varint((field * 8 + 2).toLong()) + varint(bytes.size.toLong()) + bytes
    private fun text(field: Int, value: String) = blob(field, value.toByteArray())
    private fun frame(module: String, id: String = "a".repeat(40)) = scalar(1, 0x1234) + text(6, "/data/private/owner/$module") + text(8, id) +
        text(4, "private_function_credentials") + scalar(2, 0xdeadbeef) + scalar(3, 0xcafe)
    private fun tombstone(frames: List<ByteArray> = listOf(frame("libart.so")), pid: Long = 123, uid: Long = 10001, threadId: Long = 234): ByteArray {
        val thread = scalar(1, threadId) + frames.fold(byteArrayOf()) { value, next -> value + blob(4, next) } + text(3, "SECRET_REGISTERS") + text(5, "SECRET_MEMORY")
        return scalar(5, pid) + scalar(6, 234) + scalar(7, uid) + blob(10, scalar(1, 6)) +
            text(14, "GC tried to mark invalid reference 0xSECRET holder=0xSECRET") +
            text(2, "PRIVATE_FINGERPRINT") + text(9, "PRIVATE_COMMAND") + text(17, "SECRET_MAPS") + text(19, "SECRET_FD") +
            blob(16, scalar(1, 234) + blob(2, thread))
    }
    internal fun controlledTrace(): ByteArray = tombstone()
    @Test fun ownNativeTraceYieldsOnlyAllowlistedRelativeFramesAndKnownAbortClass() {
        val result = NativeTraceSummary.parse(tombstone(List(44) { frame(if (it == 1) "libskyline.so" else "libart.so") }), 123, 10001)
        assertEquals("ok", result.status); assertEquals(6, result.signal)
        assertEquals("ART_GC_INVALID_REFERENCE", result.abortSignature)
        assertEquals(16, result.frames.size); assertEquals(44, result.framesSeen); assertEquals(28, result.framesOmitted)
        val output = result.fields().toString()
        assertTrue(output.contains("libskyline.so+0x1234@")); assertTrue(output.length < 2048)
        for (secret in listOf("/data", "owner", "private_function", "SECRET", "deadbeef", "cafe", "FINGERPRINT", "PRIVATE_COMMAND", "REGISTERS", "MAPS")) assertFalse(secret, output.contains(secret))
    }
    @Test fun unknownModulesAndInvalidBuildIdsNeverBecomeRawFallbackText() {
        val result = NativeTraceSummary.parse(tombstone(listOf(frame("private-user-library.so"), frame("libc.so", "content://secret/credential"))), 123, 10001)
        assertEquals("ok", result.status); assertEquals(1, result.frames.size); assertEquals(1, result.framesOmitted)
        assertEquals("", result.frames[0].buildId)
        assertFalse(result.fields().toString().contains("credential"))
    }
    @Test fun ownershipAndWireValidationFailClosed() {
        assertEquals("ownership_mismatch", NativeTraceSummary.parse(tombstone(uid = 10002), 123, 10001).status)
        assertEquals("ownership_mismatch", NativeTraceSummary.parse(tombstone(pid = 124), 123, 10001).status)
        assertEquals("ownership_mismatch", NativeTraceSummary.parse(tombstone(threadId = 235), 123, 10001).status)
        for (bad in listOf(tombstone() + scalar(7, 10001), byteArrayOf(0), ByteArray(11) { 0x80.toByte() }, byteArrayOf(0x12, 0x7f, 0), byteArrayOf(0x0b)))
            assertEquals("malformed_or_unsupported", NativeTraceSummary.parse(bad, 123, 10001).status)
        assertEquals("over_byte_limit", NativeTraceSummary.parse(ByteArray(NativeTraceSummary.MAX_BYTES + 1), 123, 10001).status)
        assertEquals("null_or_overwritten", NativeTraceSummary.read(null, 123, 10001).status)
        assertEquals("over_byte_limit", NativeTraceSummary.read(ByteArray(NativeTraceSummary.MAX_BYTES + 1).inputStream(), 123, 10001).status)
        val random = Random(1234)
        repeat(1000) { NativeTraceSummary.parse(ByteArray(random.nextInt(1024)).also(random::nextBytes), 123, 10001) }
    }
    @Test fun observerDeadlineCannotBeHeldByAnUninterruptibleTraceRead() {
        val release = CountDownLatch(1); val closed = CountDownLatch(1)
        val input = object : InputStream() {
            override fun read(): Int {
                while (release.count > 0) try { release.await() } catch (_: InterruptedException) { }
                return -1
            }
            override fun close() { closed.countDown() }
        }
        val start = System.nanoTime()
        try {
            PreviousProcessExit.queryBounded(100) { NativeTraceSummary.read(input, 123, 10001) }
            fail("Expected observer timeout")
        } catch (_: TimeoutException) { assertTrue(TimeUnit.NANOSECONDS.toMillis(System.nanoTime() - start) < 2000) }
        finally { release.countDown(); assertTrue(closed.await(3, TimeUnit.SECONDS)) }
    }
}
