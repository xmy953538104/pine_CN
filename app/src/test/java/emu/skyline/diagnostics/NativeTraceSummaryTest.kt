// SPDX-License-Identifier: MPL-2.0
package emu.skyline.diagnostics

import org.junit.Assert.*
import org.junit.Test
import java.io.ByteArrayOutputStream
import java.io.InputStream
import java.io.IOException
import java.io.SequenceInputStream
import java.nio.file.Files
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
        for (bad in listOf(tombstone() + scalar(7, 10001), byteArrayOf(0), ByteArray(11) { 0x80.toByte() }, byteArrayOf(0x0b)))
            assertEquals("malformed_or_unsupported", NativeTraceSummary.parse(bad, 123, 10001).status)
        assertEquals("malformed_or_unsupported", NativeTraceSummary.parse(ByteArray(NativeTraceSummary.MAX_BYTES + 1), 123, 10001).status)
        val incompleteUnknown = NativeTraceSummary.parse(byteArrayOf(0x12, 0x7f, 0), 123, 10001)
        assertEquals("ownership_unverified", incompleteUnknown.status)
        assertEquals("truncated_input", incompleteUnknown.captureStatus)
        assertEquals("null_or_overwritten", NativeTraceSummary.read(null, 123, 10001).status)
        assertEquals("malformed_or_unsupported", NativeTraceSummary.read(ByteArray(NativeTraceSummary.MAX_BYTES + 1).inputStream(), 123, 10001).status)
        val random = Random(1234)
        repeat(1000) { NativeTraceSummary.parse(ByteArray(random.nextInt(1024)).also(random::nextBytes), 123, 10001) }
    }
    @Test fun realFileBeyondOldLimitSkipsLargeUnknownFieldsBeforeAndAfterFaultThread() {
        val path = Files.createTempFile("pine-large-tombstone", ".pb")
        try {
            val skipped = blob(17, ByteArray(NativeTraceSummary.MAX_BYTES + 123) { 65 })
            Files.newOutputStream(path).use { it.write(skipped); it.write(controlledTrace()); it.write(skipped) }
            val result = NativeTraceSummary.read(Files.newInputStream(path), 123, 10001)
            assertEquals("ok", result.status); assertTrue(result.complete)
            assertTrue(result.bytesRead > NativeTraceSummary.MAX_BYTES)
            assertEquals("ART_GC_INVALID_REFERENCE", result.abortSignature)
            assertEquals(listOf("libart.so"), result.frames.map { it.module })
            assertFalse(result.fields().toString().contains("AAAA"))
        } finally { Files.deleteIfExists(path) }
    }
    @Test fun largeUnknownNestedFieldsAndUnorderedIdentityDoNotRequireWholeTraceBuffer() {
        val thread = scalar(1, 234) + blob(5, ByteArray(NativeTraceSummary.MAX_BYTES + 16)) + blob(4, frame("libskyline.so"))
        val trace = blob(16, blob(2, thread) + scalar(1, 234)) + scalar(7, 10001) + scalar(6, 234) + scalar(5, 123)
        val result = NativeTraceSummary.read(trace.inputStream(), 123, 10001)
        assertEquals("ok", result.status); assertEquals("libskyline.so", result.frames.single().module)
    }
    @Test fun scanLimitKeepsVerifiedPrefixButNeverConsumesUnlimitedUnknownBytes() {
        var consumed = 0
        val endless = object : InputStream() {
            override fun read(): Int { consumed++; return 0 }
            override fun read(bytes: ByteArray, offset: Int, length: Int): Int {
                java.util.Arrays.fill(bytes, offset, offset + length, 0.toByte()); consumed += length; return length
            }
        }
        val prefix = controlledTrace() + varint(17 * 8 + 2L) + varint(NativeTraceSummary.MAX_SCAN_BYTES + 100L)
        val result = NativeTraceSummary.read(SequenceInputStream(prefix.inputStream(), endless), 123, 10001)
        assertEquals("truncated_byte_limit", result.status); assertFalse(result.complete)
        assertEquals("libart.so", result.frames.single().module)
        assertEquals(6, result.signal); assertEquals("ART_GC_INVALID_REFERENCE", result.abortSignature)
        assertEquals(NativeTraceSummary.MAX_SCAN_BYTES, result.bytesRead)
        assertTrue(consumed <= NativeTraceSummary.MAX_SCAN_BYTES)
    }
    @Test fun incompleteTailAndIoFailurePreserveOnlyCompletedVerifiedSummaries() {
        val prefix = controlledTrace()
        val incomplete = prefix + varint(17 * 8 + 2L) + varint(99) + byteArrayOf(1)
        val truncated = NativeTraceSummary.read(incomplete.inputStream(), 123, 10001)
        assertEquals("truncated_input", truncated.status); assertEquals(1, truncated.frames.size)
        var first = true; var closed = false
        val broken = object : InputStream() {
            override fun read(): Int = throw IOException("PRIVATE_PATH")
            override fun read(bytes: ByteArray, offset: Int, length: Int): Int {
                if (!first) throw IOException("PRIVATE_PATH")
                first = false; prefix.copyInto(bytes, offset); return prefix.size
            }
            override fun close() { closed = true; throw IOException("PRIVATE_CLOSE_PATH") }
        }
        val failed = NativeTraceSummary.read(broken, 123, 10001)
        assertEquals("truncated_read_failed", failed.status); assertEquals(1, failed.frames.size); assertTrue(closed)
        assertFalse(failed.fields().toString().contains("PRIVATE"))
        val malformed = NativeTraceSummary.read(prefix.plus(byteArrayOf(0)).inputStream(), 123, 10001)
        assertEquals("malformed_or_unsupported", malformed.status); assertTrue(malformed.frames.isEmpty())
    }
    @Test fun partialCaptureRequiresObservedMatchingOwnershipAndDeadlineIsExplicit() {
        val tail = varint(17 * 8 + 2L) + varint(99) + byteArrayOf(1)
        val wrong = NativeTraceSummary.read((tombstone(uid = 10002) + tail).inputStream(), 123, 10001)
        assertEquals("ownership_mismatch", wrong.status); assertTrue(wrong.frames.isEmpty()); assertEquals(0, wrong.signal)
        val unverified = NativeTraceSummary.read((blob(16, scalar(1, 234) + blob(2, scalar(1, 234) + blob(4, frame("libart.so")))) + tail).inputStream(), 123, 10001)
        assertEquals("ownership_unverified", unverified.status); assertTrue(unverified.frames.isEmpty())
        assertEquals("truncated_input", unverified.captureStatus)
        val deadline = NativeTraceSummary.read(controlledTrace().inputStream(), 123, 10001, 0)
        assertEquals("truncated_deadline", deadline.captureStatus); assertEquals(0, deadline.bytesRead)
    }
    @Test fun malformedUtf8AfterGoodFrameFailsClosedAndFieldBudgetIsExplicit() {
        val bad = scalar(1, 1) + blob(6, byteArrayOf(0xc3.toByte(), 0x28))
        val malformed = NativeTraceSummary.parse(tombstone(listOf(frame("libc.so"), bad)), 123, 10001)
        assertEquals("malformed_or_unsupported", malformed.status); assertTrue(malformed.frames.isEmpty())
        val many = ByteArrayOutputStream().apply {
            write(controlledTrace())
            repeat(40_000) { write(scalar(22, 1)) }
        }.toByteArray()
        val limited = NativeTraceSummary.read(many.inputStream(), 123, 10001)
        assertEquals("truncated_work_limit", limited.status); assertEquals(1, limited.frames.size)
    }

    @Test fun missingRelativePcIsNotInventedAsZeroAndThreadMismatchStillFailsClosed() {
        val noPc = text(6, "/system/lib64/libc.so") + text(8, "a".repeat(40))
        val result = NativeTraceSummary.parse(tombstone(listOf(noPc)), 123, 10001)
        assertTrue(result.frames.isEmpty()); assertEquals(1, result.framesOmitted)
        val tail = varint(17 * 8 + 2L) + varint(99)
        val wrong = NativeTraceSummary.parse(tombstone(threadId = 1) + tail, 123, 10001)
        assertEquals("ownership_mismatch", wrong.status); assertTrue(wrong.frames.isEmpty())
    }

    @Test fun fieldZeroTagsAreRejectedAtEveryParsedMessageLevel() {
        for (tag in 1..7) {
            val bad = byteArrayOf(tag.toByte(), 0, 0, 0, 0, 0, 0, 0, 0)
            val identity = scalar(5, 123) + scalar(6, 234) + scalar(7, 10001)
            val cases = listOf(
                controlledTrace() + bad,
                identity + blob(10, scalar(1, 6) + bad),
                identity + blob(16, scalar(1, 234) + bad),
                identity + blob(16, scalar(1, 234) + blob(2, scalar(1, 234) + bad)),
                identity + blob(16, scalar(1, 234) + blob(2, scalar(1, 234) + blob(4, frame("libart.so") + bad))))
            for (bytes in cases) {
                val result = NativeTraceSummary.read(bytes.inputStream(), 123, 10001)
                assertEquals("tag=$tag", "malformed_or_unsupported", result.status)
                assertTrue(result.frames.isEmpty())
            }
        }
    }
    @Test fun faultFramesRequirePositiveMatchingTopMapAndThreadIdentityEvenForPartialCapture() {
        val tail = varint(17 * 8 + 2L) + varint(999) // Deliberately truncated after a complete thread.
        fun capture(top: ByteArray, key: Long, thread: ByteArray, partial: Boolean): NativeTraceSummary.Result {
            val bytes = scalar(5, 123) + scalar(7, 10001) + top + blob(16, scalar(1, key) + blob(2, thread)) + if (partial) tail else byteArrayOf()
            return NativeTraceSummary.read(bytes.inputStream(), 123, 10001)
        }
        for (partial in listOf(false, true)) {
            val frames = blob(4, frame("libart.so"))
            val rejected = listOf(
                capture(byteArrayOf(), 234, scalar(1, 234) + frames, partial),
                capture(scalar(6, 0), 0, scalar(1, 0) + frames, partial),
                capture(scalar(6, 234), 0, scalar(1, 234) + frames, partial),
                capture(scalar(6, 234), 234, frames, partial),
                capture(scalar(6, 234), 234, scalar(1, 0) + frames, partial),
                capture(scalar(6, 234), 234, frames + scalar(1, 235), partial))
            rejected.forEach { assertTrue("${it.status} must omit unverified frames", it.frames.isEmpty()) }
            val matchedLate = capture(scalar(6, 234), 234, frames + scalar(1, 234), partial)
            assertEquals(1, matchedLate.frames.size) // Ordering is allowed once all three IDs are observed.
        }
        // Stop inside the thread before its later identity can be observed. A
        // valid completed frame alone is not evidence of fault-thread ownership.
        val delayed = blob(4, frame("libart.so")) + scalar(1, 235)
        val full = scalar(5, 123) + scalar(7, 10001) + scalar(6, 234) + blob(16, scalar(1, 234) + blob(2, delayed))
        val beforeIdentity = NativeTraceSummary.read(full.copyOf(full.size - scalar(1, 235).size).inputStream(), 123, 10001)
        assertEquals("ownership_unverified", beforeIdentity.status)
        assertEquals("truncated_input", beforeIdentity.captureStatus)
        assertTrue(beforeIdentity.frames.isEmpty())
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
