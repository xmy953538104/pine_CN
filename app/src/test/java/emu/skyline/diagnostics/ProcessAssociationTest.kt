// SPDX-License-Identifier: MPL-2.0
package emu.skyline.diagnostics

import org.junit.Assert.*
import org.junit.Test
import java.nio.file.Files
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.CopyOnWriteArrayList

class ProcessAssociationTest {
    private val state = ProcessAssociation("11111111-1111-4111-8111-111111111111", "22222222-2222-4222-8222-222222222222",
        "33333333-3333-4333-8333-333333333333", "a".repeat(40), "0100751007ADA000", "emulation", true, false, "cnmt_application")
    @Test fun boundedAnonymousStateRoundTripsAndRejectsMalformedOrNewerFormats() {
        val bytes = ProcessAssociationCodec.encode(state)
        assertEquals(84, bytes.size); assertTrue(bytes.size <= 128)
        assertEquals(state, ProcessAssociationCodec.decode(bytes))
        assertEquals(state, ProcessAssociationCodec.forScope(bytes, "emulation"))
        assertNull(ProcessAssociationCodec.forScope(bytes, "main"))
        assertNull(ProcessAssociationCodec.forScope(ProcessAssociationCodec.encode(state.copy(enabled = false)), "emulation"))
        assertNull(ProcessAssociationCodec.forScope(ProcessAssociationCodec.encode(state.copy(session = "")), "emulation"))
        assertNull(ProcessAssociationCodec.decode(null))
        assertNull(ProcessAssociationCodec.decode(bytes + 0))
        assertNull(ProcessAssociationCodec.decode(bytes.copyOf(20)))
        assertNull(ProcessAssociationCodec.decode(bytes.copyOf().also { it[0] = 0 }))
        assertNull(ProcessAssociationCodec.decode(bytes.copyOf().also { it[4] = 8 }))
        val unknown = ProcessAssociationCodec.encode(state.copy(title = "content://private/secret", commit = "bad", launch = "bad"))
        val decoded = ProcessAssociationCodec.decode(unknown)!!
        assertEquals("", decoded.title); assertEquals("unknown", decoded.titleSource); assertEquals("", decoded.commit); assertEquals("", decoded.launch)
        assertFalse(String(unknown, Charsets.ISO_8859_1).contains("private"))
    }
    @Test fun legacyAndUntypedTitlesStayUnknownWhileAnonymousSessionAssociationIsRetained() {
        val legacy = ProcessAssociationCodec.encode(state).also { it[3] = 0x31; it[6] = 0 }
        val decoded = ProcessAssociationCodec.forScope(legacy, "emulation")!!
        assertEquals(state.process, decoded.process); assertEquals(state.session, decoded.session)
        assertEquals("", decoded.title); assertEquals("unknown", decoded.titleSource)
        val unknown = ProcessAssociationCodec.decode(ProcessAssociationCodec.encode(state.copy(titleSource = "unknown")))!!
        assertEquals("", unknown.title); assertEquals("unknown", unknown.titleSource)
        assertNull(ProcessAssociationCodec.decode(ProcessAssociationCodec.encode(state).also { it[6] = 99 }))
    }
    @Test fun summaryPublisherCoalescesWhilePlatformCallIsBlockedAndKeepsOwnedBytes() {
        val started = CountDownLatch(1); val release = CountDownLatch(1); val finished = CountDownLatch(1)
        val observed = CopyOnWriteArrayList<Int>()
        val publisher = StateSummaryPublisher(0, { bytes ->
            assertTrue(Thread.currentThread().isDaemon)
            observed.add(bytes!![0].toInt())
            if (observed.size == 1) { started.countDown(); release.await(3, TimeUnit.SECONDS) }
            else finished.countDown()
        })
        publisher.offer(byteArrayOf(1)); assertTrue(started.await(3, TimeUnit.SECONDS))
        val pending = byteArrayOf(2)
        repeat(10000) { publisher.offer(pending) }
        pending[0] = 99
        release.countDown(); assertTrue(finished.await(3, TimeUnit.SECONDS))
        assertEquals(listOf(1, 2), observed.toList())
    }
    @Test fun platformRejectionIsReportedWithoutBreakingFutureSummaries() {
        val failure = CountDownLatch(1); val success = CountDownLatch(1)
        val publisher = StateSummaryPublisher(0, { if (it == null) success.countDown() else throw IllegalStateException("throttled") }, { failure.countDown() })
        publisher.offer(byteArrayOf(1)); assertTrue(failure.await(3, TimeUnit.SECONDS))
        publisher.offer(null); assertTrue(success.await(3, TimeUnit.SECONDS))
    }
    @Test fun disabledStructuredEventsDoNotEvaluateFields() {
        Diagnostics.finishSession()
        var evaluated = false
        assertFalse(Diagnostics.fieldsEvent("content", "SYNTHETIC") { evaluated = true; mapOf("secret" to "not evaluated") })
        assertFalse(evaluated)
    }
    @Test fun sharedPrivateDedupStoreRejectsConcurrentAccessWithoutWaiting() {
        val root = Files.createTempDirectory("pine-exit-lock").toFile()
        try {
            ExitRecordStore(root).locked {
                val start = System.nanoTime()
                try { ExitRecordStore(root).locked { fail("Concurrent history lock acquired") }; fail("Expected contention") }
                catch (_: java.nio.channels.OverlappingFileLockException) { }
                assertTrue(TimeUnit.NANOSECONDS.toMillis(System.nanoTime() - start) < 1000)
            }
        } finally { root.deleteRecursively() }
    }
    @Test fun historicalExitIdentityIsStableAcrossImportSessionsAndPrivateStoreIsBounded() {
        val first = ExitRecordIdentity.id(123, 5, 6, 42, "emulation")
        assertEquals(first, ExitRecordIdentity.id(123, 5, 6, 42, "emulation"))
        assertNotEquals(first, ExitRecordIdentity.id(124, 5, 6, 42, "emulation"))
        assertNotEquals(first, ExitRecordIdentity.id(123, 5, 6, 42, "main"))
        val root = Files.createTempDirectory("pine-exit-store").toFile()
        try {
            val store = ExitRecordStore(root)
            store.locked { assertTrue(it.add("e:$first")) }
            store.locked { assertFalse(it.add("e:$first")); repeat(200) { n -> it.add("e:" + ExitRecordIdentity.id(n.toLong(), 5, 6, 42, "emulation")) } }
            store.locked { assertEquals(128, it.size) }
            assertTrue(java.io.File(root, "exit-records.txt").length() < 16384)
        } finally { root.deleteRecursively() }
    }
}
