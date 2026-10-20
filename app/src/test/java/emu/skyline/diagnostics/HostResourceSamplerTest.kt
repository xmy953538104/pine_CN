// SPDX-License-Identifier: MPL-2.0
package emu.skyline.diagnostics

import org.junit.Assert.*
import org.junit.Test
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference

class HostResourceSamplerTest {
    private fun token(session: String = "A", launch: String = "launch", scope: String = "main") =
        HostResourceSampler.Identity(session, launch, 1, "process", scope, "unspecified")
    private val value = HostResourceSampler.Reading(123, 456)

    @Test fun disabledAndInvalidLaunchNeverReadOrPublish() {
        var calls = 0
        val sampler = HostResourceSampler({ 0 }, { calls++; value }, { _, _ -> calls++; true })
        repeat(10) { assertEquals(HostResourceSampler.Result.DISABLED, sampler.poll()) }
        sampler.update(token(launch = ""))
        assertEquals(HostResourceSampler.Result.DISABLED, sampler.poll())
        assertEquals(0, calls)
    }
    @Test fun minimumFiveSecondsNoBurstOrCatchupAndDebugMainAllowed() {
        var now = 0L
        var calls = 0
        val sampler = HostResourceSampler({ now }, { calls++; value }, { identity, _ ->
            assertEquals("main", identity.scope); true
        }, intervalMs = 1)
        sampler.update(token())
        now = 4_999_000_000
        assertEquals(HostResourceSampler.Result.NOT_DUE, sampler.poll())
        now = 5_000_000_000
        assertEquals(HostResourceSampler.Result.ACCEPTED, sampler.poll())
        now = 99_000_000_000
        assertEquals(HostResourceSampler.Result.ACCEPTED, sampler.poll())
        assertEquals(HostResourceSampler.Result.NOT_DUE, sampler.poll())
        assertEquals(2, calls)
    }
    @Test fun replacementDuringReadDropsOldSessionAndNoConcurrentReads() {
        var now = 0L
        val entered = CountDownLatch(1)
        val release = CountDownLatch(1)
        var published = 0
        val sampler = HostResourceSampler({ now }, {
            entered.countDown(); check(release.await(5, TimeUnit.SECONDS)); value
        }, { _, _ -> published++; true })
        sampler.update(token())
        now = 10_000_000_000
        val result = AtomicReference<HostResourceSampler.Result>()
        val worker = Thread { result.set(sampler.poll()) }
        worker.start()
        try {
            assertTrue(entered.await(5, TimeUnit.SECONDS))
            assertEquals(HostResourceSampler.Result.BUSY, sampler.poll())
            sampler.update(token("B")) // Must not wait for the OS memory read.
        } finally { release.countDown(); worker.join(5_000) }
        assertFalse(worker.isAlive)
        assertEquals(HostResourceSampler.Result.STALE, result.get())
        assertEquals(0, published)
    }
    @Test fun stopAndReenableSameIdentityStillInvalidatesInFlightRead() {
        var now = 0L
        lateinit var sampler: HostResourceSampler
        sampler = HostResourceSampler({ now }, {
            sampler.update(null); sampler.update(token()); value
        }, { _, _ -> fail("old epoch cannot publish"); true })
        sampler.update(token()); now = 10_000_000_000
        assertEquals(HostResourceSampler.Result.STALE, sampler.poll())
    }
    @Test fun slowOrUnavailableReadsBackOffAndHealthyReadRecovers() {
        var now = 0L
        var slow = true
        var last: HostResourceSampler.Sample? = null
        val sampler = HostResourceSampler({ now }, {
            if (slow) now += 101_000_000
            value
        }, { _, sample -> last = sample; true })
        sampler.update(token())
        now = 10_000_000_000; sampler.poll()
        assertTrue(last!!.overBudget); assertEquals(20_000, last!!.nextDelayMs)
        now += 20_000_000_000; sampler.poll()
        assertEquals(40_000, last!!.nextDelayMs)
        now += 40_000_000_000; sampler.poll()
        assertEquals(60_000, last!!.nextDelayMs)
        slow = false; now += 60_000_000_000; sampler.poll()
        assertFalse(last!!.overBudget); assertEquals(10_000, last!!.nextDelayMs)
    }
    @Test fun readFailureIsUnavailableAndPublishFailureDoesNotEscape() {
        var now = 0L
        val sampler = HostResourceSampler({ now }, { throw IllegalStateException("private") }, { _, sample ->
            assertNull(sample.reading.pssKib); assertNull(sample.reading.rssKib)
            assertEquals("read_failed", sample.reading.pssStatus)
            assertEquals(20_000, sample.nextDelayMs)
            throw IllegalStateException("sink off")
        })
        sampler.update(token()); now = 10_000_000_000
        assertEquals(HostResourceSampler.Result.REJECTED, sampler.poll())
        assertEquals(HostResourceSampler.Result.NOT_DUE, sampler.poll())
    }
    @Test fun finalAdmissionMayRejectStoppedNativeSession() {
        var now = 0L
        val sampler = HostResourceSampler({ now }, { value }, { _, _ -> false })
        sampler.update(token()); now = 10_000_000_000
        assertEquals(HostResourceSampler.Result.REJECTED, sampler.poll())
    }
    @Test fun rssParserRequiresOneExactFieldUnitsAsciiAndBoundedInput() {
        assertEquals(123L, HostResourceSampler.parseRssKib("Name:\tPine\nVmRSS:\t123 kB\nThreads:\t5\n"))
        assertEquals(0L, HostResourceSampler.parseRssKib("VmRSS: 0 kB"))
        listOf("", "VmRSS: -1 kB", "VmRSS: 1 MB", "VmRSS: ١ kB", "VmRSS: 1 kB\nVmRSS: 2 kB",
            "VmRSS: 9999999999999999999999 kB", "x".repeat(65_537)).forEach {
            assertNull(HostResourceSampler.parseRssKib(it))
        }
    }
}
