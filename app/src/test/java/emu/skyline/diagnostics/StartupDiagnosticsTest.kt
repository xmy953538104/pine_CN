// SPDX-License-Identifier: MPL-2.0
package emu.skyline.diagnostics

import android.app.ApplicationExitInfo
import org.junit.Assert.*
import org.junit.Test
import java.util.concurrent.CountDownLatch
import java.util.concurrent.ExecutionException
import java.util.concurrent.TimeUnit
import java.util.concurrent.TimeoutException

class StartupDiagnosticsTest {
    @Test fun historyQueryReturnsOriginalReadFailureAndRunsOnDaemonWorker() {
        val expected = IllegalStateException("synthetic unavailable service")
        try {
            PreviousProcessExit.queryBounded(1000) {
                assertTrue(Thread.currentThread().isDaemon)
                assertEquals("PineExitQuery", Thread.currentThread().name)
                throw expected
            }
            fail("Expected service read failure")
        } catch (error: ExecutionException) {
            assertSame(expected, error.cause)
        }
        assertEquals(7, PreviousProcessExit.queryBounded(1000) { 7 })
    }

    @Test fun uninterruptibleHistoryQueryCannotHoldTheObserverPastItsDeadline() {
        val release = CountDownLatch(1)
        val finished = CountDownLatch(1)
        val started = System.nanoTime()
        try {
            PreviousProcessExit.queryBounded(200) {
                try {
                    // A Binder call need not honor interruption. Release this
                    // controlled worker explicitly after observing the timeout.
                    while (release.count > 0) {
                        try { release.await() } catch (_: InterruptedException) { }
                    }
                } finally { finished.countDown() }
            }
            fail("Expected bounded history query timeout")
        } catch (_: TimeoutException) {
            assertTrue(TimeUnit.NANOSECONDS.toMillis(System.nanoTime() - started) < 3000)
        } finally {
            release.countDown()
            assertTrue(finished.await(3, TimeUnit.SECONDS))
        }
    }

    @Test fun titleIdentityIsBoundedStructuredHexInsteadOfDetailParsing() {
        assertEquals("0100000000001000", Diagnostics.normalizedTitleId("0100000000001000"))
        assertEquals("010000000000ABCD", Diagnostics.normalizedTitleId("010000000000abcd"))
        for (invalid in listOf(null, "", "unknown", "title_id=0100000000001000", "../private", "01000000000010000"))
            assertEquals("", Diagnostics.normalizedTitleId(invalid))
    }

    @Test fun systemReportsCrashesAndAnrWithoutRelabelingNormalExitOrSignalKill() {
        for (reason in listOf(ApplicationExitInfo.REASON_CRASH, ApplicationExitInfo.REASON_CRASH_NATIVE, ApplicationExitInfo.REASON_ANR))
            assertEquals("PREVIOUS_PROCESS_CRASH", PreviousProcessExit.eventName(reason))
        for (reason in listOf(ApplicationExitInfo.REASON_EXIT_SELF, ApplicationExitInfo.REASON_SIGNALED,
            ApplicationExitInfo.REASON_USER_REQUESTED, ApplicationExitInfo.REASON_LOW_MEMORY, ApplicationExitInfo.REASON_UNKNOWN))
            assertEquals("PREVIOUS_PROCESS_EXIT", PreviousProcessExit.eventName(reason))
    }
}
