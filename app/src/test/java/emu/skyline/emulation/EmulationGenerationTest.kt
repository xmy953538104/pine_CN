// SPDX-License-Identifier: MPL-2.0
package emu.skyline.emulation

import java.util.Collections
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import kotlinx.coroutines.*
import org.junit.Assert.*
import org.junit.Test

class EmulationGenerationTest {
    private fun CountDownLatch.awaitReady() = assertTrue("worker reached checkpoint", await(5, TimeUnit.SECONDS))

    private fun scenario(test: suspend CoroutineScope.(EmulationGeneration, ExecutorCoroutineDispatcher, ExecutorCoroutineDispatcher) -> Unit) = runBlocking {
        val worker = Executors.newSingleThreadExecutor().asCoroutineDispatcher()
        val completion = Executors.newSingleThreadExecutor().asCoroutineDispatcher()
        try { test(EmulationGeneration(worker, completion), worker, completion) }
        finally { worker.close(); completion.close() }
    }

    @Test fun delayedOldDrainCannotStopReplacement() = scenario { owner, worker, _ ->
        val entered = CountDownLatch(1); val release = CountDownLatch(1)
        val barrier = launch(worker) { entered.countDown(); release.awaitReady() }
        entered.awaitReady()
        val events = Collections.synchronizedList(mutableListOf<String>())
        val a = owner.begin()
        val ending = launch(start = CoroutineStart.UNDISPATCHED) { owner.finish(a, { events.add("drain A") }, { events.add("exit A") }) }
        val b = owner.begin()
        release.countDown(); barrier.join(); ending.join()
        owner.session(b) { events.add("apply B") }
        assertEquals(listOf("apply B"), events)
    }

    @Test fun replacementDuringDrainWaitsForOldDrainAndCannotBeKilled() = scenario { owner, _, _ ->
        val entered = CountDownLatch(1); val release = CountDownLatch(1)
        val events = Collections.synchronizedList(mutableListOf<String>())
        val a = owner.begin()
        val ending = launch(start = CoroutineStart.UNDISPATCHED) {
            owner.finish(a, { entered.countDown(); release.awaitReady(); events.add("drain A") }, { events.add("exit A") })
        }
        entered.awaitReady()
        val b = owner.begin() // Must not wait for the drain or block the UI thread.
        val attempted = CountDownLatch(1)
        val applying = Thread {
            attempted.countDown()
            owner.session(b) { events.add("apply B") }
        }.apply { start() }
        try {
            attempted.awaitReady()
            val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(3)
            while (applying.state != Thread.State.BLOCKED && applying.isAlive && System.nanoTime() < deadline)
                Thread.sleep(1)
            assertEquals("B must actually be waiting for the owned session lock", Thread.State.BLOCKED, applying.state)
            assertTrue(events.isEmpty())
        } finally {
            release.countDown()
            ending.join()
            applying.join(5000)
        }
        assertFalse("B apply thread completed", applying.isAlive)
        assertEquals(listOf("drain A", "apply B"), events)
    }

    @Test fun replacementDuringApplyRejectsOldNativeExecutionAfterActionReturns() = scenario { owner, _, _ ->
        val entered = CountDownLatch(1); val release = CountDownLatch(1)
        val events = Collections.synchronizedList(mutableListOf<String>())
        val accepted = java.util.concurrent.atomic.AtomicBoolean(true)
        val a = owner.begin()
        val applying = Thread {
            val result = owner.session(a) {
                entered.countDown()
                release.awaitReady()
                events.add("apply A returned")
            }
            accepted.set(result)
            if (result) events.add("execute native A")
        }.apply { start() }
        var b = 0L
        try {
            entered.awaitReady()
            b = owner.begin()
        } finally {
            release.countDown()
            applying.join(5000)
        }
        assertFalse("A apply thread completed", applying.isAlive)
        assertFalse("Post-action generation check rejects A", accepted.get())
        assertTrue(owner.session(b) { events.add("apply B") })
        assertEquals(listOf("apply A returned", "apply B"), events)
    }

    @Test fun replacementAfterDrainBeforeNavigationSuppressesOldExit() = scenario { owner, _, completion ->
        val entered = CountDownLatch(1); val release = CountDownLatch(1); val drained = CountDownLatch(1)
        val barrier = launch(completion) { entered.countDown(); release.awaitReady() }
        entered.awaitReady()
        var exited = false
        val a = owner.begin()
        val ending = launch(start = CoroutineStart.UNDISPATCHED) { owner.finish(a, { drained.countDown() }, { exited = true }) }
        drained.awaitReady()
        val b = owner.begin()
        release.countDown(); barrier.join(); ending.join()
        assertFalse(exited); assertTrue(owner.isCurrent(b))
    }

    @Test fun callerCancellationCannotLoseAcceptedNormalEnd() = scenario { owner, _, _ ->
        val entered = CountDownLatch(1); val release = CountDownLatch(1)
        val events = Collections.synchronizedList(mutableListOf<String>())
        val activityJob = Job()
        val activityScope = CoroutineScope(activityJob + Dispatchers.Unconfined)
        val a = owner.begin()
        val ending = activityScope.launch(start = CoroutineStart.UNDISPATCHED) {
            owner.finish(a, { entered.countDown(); release.awaitReady(); events.add("drain") }, { events.add("exit") })
        }
        entered.awaitReady(); activityJob.cancel(); release.countDown(); ending.join()
        assertEquals(listOf("drain", "exit"), events)
    }

    @Test fun cancellationBeforeDrainStillCompletesAcceptedEnd() = scenario { owner, worker, _ ->
        val entered = CountDownLatch(1); val release = CountDownLatch(1)
        val barrier = launch(worker) { entered.countDown(); release.awaitReady() }
        entered.awaitReady()
        val events = Collections.synchronizedList(mutableListOf<String>())
        val a = owner.begin()
        val ending = launch(start = CoroutineStart.UNDISPATCHED) { owner.finish(a, { events.add("drain") }, { events.add("exit") }) }
        ending.cancel(); release.countDown(); barrier.join(); ending.join()
        assertEquals(listOf("drain", "exit"), events)
    }

    @Test fun cancellationAndReplacementStillSuppressOldExit() = scenario { owner, _, _ ->
        val entered = CountDownLatch(1); val release = CountDownLatch(1)
        val a = owner.begin(); var exited = false
        val ending = launch(start = CoroutineStart.UNDISPATCHED) {
            owner.finish(a, { entered.countDown(); release.awaitReady() }, { exited = true })
        }
        entered.awaitReady(); ending.cancel(); val b = owner.begin()
        release.countDown(); ending.join()
        assertFalse(exited); assertTrue(owner.isCurrent(b))
    }

    @Test fun duplicateEndRequestsDrainAndExitExactlyOnce() = scenario { owner, _, _ ->
        val a = owner.begin(); val events = Collections.synchronizedList(mutableListOf<String>())
        val jobs = (1..8).map { launch(start = CoroutineStart.UNDISPATCHED) { owner.finish(a, { events.add("drain") }, { events.add("exit") }) } }
        jobs.joinAll()
        assertEquals(listOf("drain", "exit"), events)
        owner.finish(a, { fail("duplicate drain") }, { fail("duplicate exit") })
    }

    @Test fun acceptedAndCompletedEndRejectLateSameGameApply() = scenario { owner, worker, _ ->
        val entered = CountDownLatch(1); val release = CountDownLatch(1)
        val barrier = launch(worker) { entered.countDown(); release.awaitReady() }
        entered.awaitReady()
        val events = Collections.synchronizedList(mutableListOf<String>())
        val a = owner.begin()
        val ending = launch(start = CoroutineStart.UNDISPATCHED) { owner.finish(a, { events.add("drain") }, { events.add("exit") }) }
        assertFalse(owner.session(a) { fail("apply after accepted ending") })
        release.countDown(); barrier.join(); ending.join()
        assertFalse(owner.session(a) { fail("apply after completed ending") })
        assertEquals(listOf("drain", "exit"), events)
    }

    @Test fun staleSessionApplyAndOldDialogRequestAreIgnored() = scenario { owner, _, _ ->
        val a = owner.begin(); val b = owner.begin()
        assertFalse(owner.session(a) { fail("old apply") })
        owner.finish(a, { fail("old dialog drain") }, { fail("old dialog exit") })
        assertTrue(owner.isCurrent(b))
    }

    @Test fun destructionBeforeAcceptanceRejectsNewFinishAndSession() = scenario { owner, _, _ ->
        val a = owner.begin()
        assertTrue(owner.destroy(a))
        assertTrue(owner.ownsNativeGame(a))
        assertFalse(owner.isCurrent(a))
        assertFalse(owner.session(a) { fail("apply after destruction") })
        owner.finish(a, { fail("new drain after destruction") }, { fail("new exit after destruction") })
        assertFalse(owner.destroy(a))
    }

    @Test fun acceptedFinishKeepsItsQueuedDrainAfterDestruction() = scenario { owner, worker, _ ->
        val entered = CountDownLatch(1); val release = CountDownLatch(1)
        val barrier = launch(worker) { entered.countDown(); release.awaitReady() }
        entered.awaitReady()
        val events = Collections.synchronizedList(mutableListOf<String>())
        val a = owner.begin()
        val ending = launch(start = CoroutineStart.UNDISPATCHED) { owner.finish(a, { events.add("drain") }, { events.add("exit") }) }
        assertTrue(owner.destroy(a))
        release.countDown(); barrier.join(); ending.join()
        assertEquals(listOf("drain", "exit"), events)
    }

    @Test fun staleDestructionDoesNotSuppressReplacement() = scenario { owner, _, _ ->
        val a = owner.begin(); val b = owner.begin()
        assertFalse(owner.destroy(a))
        assertTrue(owner.isCurrent(b))
        assertFalse(owner.ownsNativeGame(a))
        assertTrue(owner.ownsNativeGame(b))
        assertTrue(owner.session(b) {})
        var exits = 0
        owner.finish(b, {}, { exits++ })
        assertEquals(1, exits)
    }
}
