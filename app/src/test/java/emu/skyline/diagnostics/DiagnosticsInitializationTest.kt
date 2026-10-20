// SPDX-License-Identifier: MPL-2.0
package emu.skyline.diagnostics

import android.app.Application
import android.content.Context
import android.content.ContextWrapper
import android.content.SharedPreferences
import java.io.IOException
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference
import emu.skyline.emulation.EmulationGeneration
import kotlinx.coroutines.*
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import org.robolectric.annotation.Implementation
import org.robolectric.annotation.Implements

/** Real Diagnostics ordering and state; only JNI side effects are shadowed. */
@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, manifest = Config.NONE, sdk = [29],
    shadows = [DiagnosticsInitializationTest.NativeDiagnostics::class],
    instrumentedPackages = ["emu.skyline.diagnostics.Diagnostics"])
class DiagnosticsInitializationTest {
    private data class Applied(val enabled: Boolean, val verbose: Boolean, val files: Int, val categories: String, val thread: String, val session: String)
    private data class NativeEvent(val name: String, val session: String, val fields: Map<String, String> = emptyMap())
    @Implements(value = Diagnostics::class, isInAndroidSdk = false)
    class NativeDiagnostics {
        companion object {
            @JvmStatic @Implementation fun configureNative(path: String, enabled: Boolean, verbose: Boolean,
                files: Int, session: String, categories: String, process: String): Boolean {
                nativeEnabled = enabled
                nativeSession = if (enabled) session else ""
                applied.add(Applied(enabled, verbose, files, categories, Thread.currentThread().name, nativeSession))
                configured.countDown()
                return enabled
            }
            @JvmStatic @Implementation fun configureResourceNative(enabled: Boolean, cohort: String) {
                if (resourceConfigureFails) throw UnsatisfiedLinkError("fixture JNI unavailable")
                resourceConfigurations.add(enabled to cohort)
            }
            @JvmStatic @Implementation fun statusNative() = longArrayOf(if (nativeEnabled) 1 else 0, 0, 0, 0)
            @JvmStatic @Implementation fun setLaunchNative(id: String, title: String, newLaunch: Boolean) = Unit
            @JvmStatic @Implementation fun emitFieldsNative(category: String, event: String, level: String,
                keys: Array<String>, values: Array<String>): Boolean {
                events.add(NativeEvent(event, nativeSession, keys.zip(values).toMap()))
                return true
            }
            @JvmStatic @Implementation fun emitNative(category: String, event: String, detail: String, title: String) {
                events.add(NativeEvent(event, nativeSession))
            }
            @JvmStatic @Implementation fun stopNative() {
                events.add(NativeEvent("STOP", nativeSession))
                nativeEnabled = false
                nativeSession = ""
            }
            @JvmStatic @Implementation fun flushNative() = true
        }
    }
    companion object {
        private val resourceConfigurations = CopyOnWriteArrayList<Pair<Boolean, String>>()
        private val applied = CopyOnWriteArrayList<Applied>()
        private val events = CopyOnWriteArrayList<NativeEvent>()
        @Volatile private var resourceConfigureFails = false
        @Volatile private var nativeEnabled = false
        @Volatile private var nativeSession = ""
        @Volatile private var configured = CountDownLatch(1)
    }

    @Test fun resourceOptInCannotEnableDisabledDiagnostics() {
        Diagnostics.apply(SnapshotContext(RuntimeEnvironment.getApplication()) {
            mapOf("enabled" to false, "resources" to true, "resource_cohort" to "official")
        })
        assertFalse(nativeEnabled)
        assertEquals(false to "official", resourceConfigurations.last())
    }
    @Test fun resourceOptInAndCohortParticipateInConfigurationSignature() {
        fun apply(resources: Boolean, cohort: String) = Diagnostics.apply(SnapshotContext(RuntimeEnvironment.getApplication()) {
            mapOf("enabled" to true, "resources" to resources, "resource_cohort" to cohort)
        })
        apply(false, "unspecified")
        val firstSession = nativeSession
        assertEquals(false to "unspecified", resourceConfigurations.last())
        apply(true, "official")
        assertNotEquals(firstSession, nativeSession)
        val officialSession = nativeSession
        assertEquals(true to "official", resourceConfigurations.last())
        apply(true, "golden_mod")
        assertNotEquals(officialSession, nativeSession)
        assertEquals(true to "golden_mod", resourceConfigurations.last())
        Diagnostics.finishSession()
        assertFalse(resourceConfigurations.last().first)
    }
    private fun assertResourceWorkerAbsentAndReadInactive() {
        val worker = Diagnostics::class.java.getDeclaredField("resourceWorker").apply { isAccessible = true }
        assertNull("No sampling thread may be created", worker.get(Diagnostics))
        val read = Diagnostics::class.java.getDeclaredMethod("readHostResources").apply { isAccessible = true }
        val value = read.invoke(Diagnostics) as HostResourceSampler.Reading
        assertEquals("inactive", value.pssStatus)
        assertEquals("inactive", value.rssStatus)
    }
    @Test fun resourceCategoryMustBeSelectedExactlyBeforeStartingOwnedLaunchSampler() {
        for (categories in listOf("content,storage", "none", "resources", "guest, resource")) {
            Diagnostics.apply(SnapshotContext(RuntimeEnvironment.getApplication()) {
                mapOf("enabled" to true, "resources" to true, "categories" to categories)
            })
            Diagnostics.beginLaunch(null, 42)
            assertEquals(false to "unspecified", resourceConfigurations.last())
            assertResourceWorkerAbsentAndReadInactive()
            Diagnostics.finishSession(42)
        }
    }
    @Test fun unavailableResourceJniNeverStartsSamplerEvenForOwnedLaunch() {
        resourceConfigureFails = true
        Diagnostics.apply(SnapshotContext(RuntimeEnvironment.getApplication()) {
            mapOf("enabled" to true, "resources" to true, "categories" to "guest,resource")
        })
        Diagnostics.beginLaunch(null, 42)
        assertTrue("Other diagnostics remain usable", nativeEnabled)
        assertResourceWorkerAbsentAndReadInactive()
        Diagnostics.finishSession(42)
    }

    @Test fun cohortIsStrictUserEnumAndMissingOptInIsOff() {
        Diagnostics.apply(SnapshotContext(RuntimeEnvironment.getApplication()) {
            mapOf("enabled" to true, "resource_cohort" to "/data/user/private-label")
        })
        assertEquals(false to "unspecified", resourceConfigurations.last())
    }

    private class SnapshotContext(base: Context, values: () -> Map<String, *>) : ContextWrapper(base) {
        private val prefs = object : SharedPreferences by base.getSharedPreferences("fixture", Context.MODE_PRIVATE) {
            override fun getAll(): MutableMap<String, *> = values().toMutableMap()
            override fun getBoolean(key: String?, defValue: Boolean): Boolean = error("Scalar read would reintroduce startup waiting")
            override fun getInt(key: String?, defValue: Int): Int = error("Expected one coherent snapshot")
            override fun getString(key: String?, defValue: String?): String? = error("Expected one coherent snapshot")
        }
        override fun getApplicationContext(): Context = this
        override fun getSharedPreferences(name: String, mode: Int): SharedPreferences {
            check(name == "diagnostics")
            return prefs
        }
    }
    private fun context(enabled: Boolean, files: Int = 4, verbose: Boolean = false, categories: String = "") =
        SnapshotContext(RuntimeEnvironment.getApplication()) {
            mapOf("enabled" to enabled, "files" to files, "verbose" to verbose, "categories" to categories)
        }

    @Before fun reset() {
        Diagnostics.finishSession()
        applied.clear()
        resourceConfigurations.clear()
        resourceConfigureFails = false
        events.clear()
        configured = CountDownLatch(1)
    }
    @After fun stop() { Diagnostics.finishSession() }

    @Test fun startupReturnsAndMonitorRemainsAvailableWhilePreferencesAreBlocked() {
        val entered = CountDownLatch(1)
        val release = CountDownLatch(1)
        val reader = SnapshotContext(RuntimeEnvironment.getApplication()) {
            entered.countDown()
            check(release.await(5, TimeUnit.SECONDS))
            mapOf("enabled" to true)
        }
        try {
            // Uses the production daemon-thread scheduler, not an inline stub.
            Diagnostics.initializeAsync(reader)
            assertTrue(entered.await(2, TimeUnit.SECONDS))
            assertFalse(nativeEnabled)
            assertTrue(applied.isEmpty())
            assertFalse(Diagnostics.fieldsEvent("test", "MUST_REMAIN_DISABLED") { error("No formatting while pending") })
            val queried = CountDownLatch(1)
            val query = Thread { Diagnostics.currentAssociation(); queried.countDown() }.apply { start() }
            assertTrue("Preference I/O must not own the Diagnostics monitor", queried.await(1, TimeUnit.SECONDS))
            query.join(1000)
            release.countDown()
            assertTrue(configured.await(2, TimeUnit.SECONDS))
            Diagnostics.currentAssociation() // Wait for publication of metadata under the monitor.
            assertTrue(nativeEnabled)
            assertEquals("PineDiagPrefs", applied.single().thread)
        } finally { release.countDown() }
    }

    @Test fun newerDisableCannotBeOverwrittenByLateEnabledStartup() {
        lateinit var late: () -> Unit
        Diagnostics.initializeAsync(context(true)) { late = it }
        Diagnostics.apply(context(false))
        late()
        assertEquals(listOf(false), applied.map { it.enabled })
        assertFalse(nativeEnabled)
    }

    @Test fun newerEnableCannotBeOverwrittenByLateDisabledStartup() {
        lateinit var late: () -> Unit
        Diagnostics.initializeAsync(context(false)) { late = it }
        Diagnostics.apply(context(true))
        late()
        assertEquals(listOf(true), applied.map { it.enabled })
        assertTrue(nativeEnabled)
    }

    @Test fun aNewerRequestWinsEvenBeforeItsPreferenceReadFinishes() {
        lateinit var late: () -> Unit
        Diagnostics.initializeAsync(context(true)) { late = it }
        val entered = CountDownLatch(1)
        val release = CountDownLatch(1)
        val latest = SnapshotContext(RuntimeEnvironment.getApplication()) {
            entered.countDown()
            check(release.await(5, TimeUnit.SECONDS))
            mapOf("enabled" to false)
        }
        val worker = Thread { Diagnostics.apply(latest) }.apply { start() }
        try {
            assertTrue(entered.await(2, TimeUnit.SECONDS))
            late()
            assertTrue(applied.isEmpty())
            assertFalse(nativeEnabled)
        } finally { release.countDown(); worker.join(3000) }
        assertFalse(worker.isAlive)
        assertEquals(listOf(false), applied.map { it.enabled })
    }

    @Test fun finishingSessionInvalidatesPendingStartup() {
        lateinit var late: () -> Unit
        Diagnostics.initializeAsync(context(true)) { late = it }
        Diagnostics.finishSession()
        late()
        assertTrue(applied.isEmpty())
        assertFalse(nativeEnabled)
    }

    @Test fun staleOwnerCannotCancelCurrentPendingConfiguration() {
        Diagnostics.beginLaunch("0100000000001000", 22)
        lateinit var late: () -> Unit
        Diagnostics.initializeAsync(context(true)) { late = it }
        Diagnostics.finishSession(11)
        late()
        assertEquals(listOf(true), applied.map { it.enabled })
        assertTrue(nativeEnabled)
    }

    @Test fun oneSnapshotPreservesAllSettingsWithoutScalarReads() {
        Diagnostics.apply(context(true, 7, true, "content,guest"))
        val value = applied.single()
        assertTrue(value.enabled)
        assertTrue(value.verbose)
        assertEquals(7, value.files)
        assertEquals("content,guest", value.categories)
    }

    @Test fun preferenceReadAndSchedulerFailuresStayDisabledAndRemainRetryable() {
        val unreadable = SnapshotContext(RuntimeEnvironment.getApplication()) { throw IOException("controlled prefs read failure") }
        Diagnostics.initializeAsync(unreadable) { it() }
        assertFalse(nativeEnabled)
        Diagnostics.initializeAsync(context(true)) { throw IllegalStateException("controlled scheduler rejection") }
        assertFalse(nativeEnabled)
        Diagnostics.apply(context(true))
        assertTrue(nativeEnabled)
        assertFalse(applied.dropLast(1).any { it.enabled })
    }

    @Test fun invalidPreferenceTypesCannotEnableOrBroadenCollection() {
        for (invalid in listOf(mapOf("enabled" to "true"),
            mapOf("enabled" to true, "categories" to setOf("guest")),
            mapOf("enabled" to true, "files" to "8"),
            mapOf("enabled" to true, "verbose" to "true"))) {
            val context = SnapshotContext(RuntimeEnvironment.getApplication()) { invalid }
            Diagnostics.initializeAsync(context) { it() }
            assertFalse(nativeEnabled)
        }
        assertFalse(applied.any { it.enabled })
    }

    /** The production prepare/owned-session wiring from EmulationActivity, with real Diagnostics and generation locks. */
    private fun replacementDuringOwnedDrain(readPending: Boolean) = runBlocking {
        val worker = Executors.newSingleThreadExecutor().asCoroutineDispatcher()
        val completion = Executors.newSingleThreadExecutor().asCoroutineDispatcher()
        val drainEntered = CountDownLatch(1)
        val drainRelease = CountDownLatch(1)
        val readEntered = CountDownLatch(1)
        val readRelease = CountDownLatch(1)
        val prepared = CountDownLatch(1)
        val workerFailure = AtomicReference<Throwable?>()
        var replacement: Thread? = null
        try {
            val generations = EmulationGeneration(worker, completion)
            val a = generations.begin()
            Diagnostics.apply(context(true))
            assertTrue(generations.session(a) { Diagnostics.beginLaunch("0100000000001000", a) })
            val sessionA = nativeSession
            val ending = launch(start = CoroutineStart.UNDISPATCHED) {
                generations.finish(a, {
                    drainEntered.countDown()
                    check(drainRelease.await(5, TimeUnit.SECONDS))
                    Diagnostics.finishSession(a)
                }, { error("Replaced A must not exit B's process") })
            }
            assertTrue(drainEntered.await(2, TimeUnit.SECONDS))
            val b = generations.begin() // A already owns its drain; B invalidates A's eventual exit.
            val reader = if (readPending) SnapshotContext(RuntimeEnvironment.getApplication()) {
                readEntered.countDown()
                check(readRelease.await(5, TimeUnit.SECONDS))
                mapOf("enabled" to true)
            } else context(true)
            replacement = Thread {
                try {
                    val begin = Diagnostics.prepareLaunch(reader, "0100000000002000", b)
                    prepared.countDown()
                    check(generations.session(b, begin))
                } catch (failure: Throwable) { workerFailure.set(failure) }
            }.apply { start() }
            if (readPending) {
                assertTrue(readEntered.await(2, TimeUnit.SECONDS))
                drainRelease.countDown()
                ending.join()
                assertFalse(nativeEnabled)
                readRelease.countDown()
            } else {
                assertTrue(prepared.await(2, TimeUnit.SECONDS))
                val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(2)
                while (replacement.state != Thread.State.BLOCKED && replacement.isAlive && System.nanoTime() < deadline)
                    Thread.sleep(1)
                assertEquals("B waits for A's already-owned session", Thread.State.BLOCKED, replacement.state)
                drainRelease.countDown()
                ending.join()
            }
            replacement.join(3000)
            assertFalse("Replacement worker completed", replacement.isAlive)
            workerFailure.get()?.let { throw AssertionError("Replacement launch failed", it) }
            val association = Diagnostics.currentAssociation()
            assertTrue("B's persisted enabled configuration must survive A's drain", nativeEnabled)
            assertTrue(association.enabled)
            assertTrue(association.startObserved)
            assertTrue(association.launch.isNotEmpty())
            assertNotEquals(sessionA, association.session)
            assertTrue(events.any { it.name == "SESSION_METADATA" && it.session == association.session &&
                it.fields["session_id"] == association.session })
            assertTrue(events.any { it.name == "LAUNCH_BEGIN" && it.session == association.session })
        } finally {
            drainRelease.countDown()
            readRelease.countDown()
            replacement?.join(3000)
            worker.close()
            completion.close()
        }
    }

    @Test fun preparedReplacementPublishesAfterAnAlreadyOwnedOldDrain() = replacementDuringOwnedDrain(false)
    @Test fun replacementPreferenceReadCrossingOldFinishStillPublishesForItsOwnedLaunch() = replacementDuringOwnedDrain(true)

    @Test fun blockedPreparationLeavesBothMonitorsAvailableAndCancellationPreventsPublication() {
        val generations = EmulationGeneration()
        val token = generations.begin()
        val entered = CountDownLatch(1)
        val release = CountDownLatch(1)
        val prepared = AtomicReference<(() -> Unit)?>()
        val reader = SnapshotContext(RuntimeEnvironment.getApplication()) {
            entered.countDown()
            check(release.await(5, TimeUnit.SECONDS))
            mapOf("enabled" to true)
        }
        val worker = Thread { prepared.set(Diagnostics.prepareLaunch(reader, "0100000000002000", token)) }.apply { start() }
        try {
            assertTrue(entered.await(2, TimeUnit.SECONDS))
            val cancelled = CountDownLatch(1)
            val ui = Thread {
                Diagnostics.currentAssociation()
                check(generations.destroy(token))
                cancelled.countDown()
            }.apply { start() }
            assertTrue("Blocked preference read must own neither monitor", cancelled.await(1, TimeUnit.SECONDS))
            ui.join(1000)
            assertFalse(nativeEnabled)
            assertTrue(applied.isEmpty())
        } finally { release.countDown(); worker.join(3000) }
        assertFalse(worker.isAlive)
        assertFalse(generations.session(token, checkNotNull(prepared.get())))
        assertTrue(applied.isEmpty())
        assertFalse(nativeEnabled)
    }

    @Test fun aNewerCompletedUserConfigurationIsPreservedAcrossOldFinishAndPreparedLaunch() {
        for (enabled in listOf(false, true)) {
            Diagnostics.finishSession()
            val generations = EmulationGeneration()
            val a = generations.begin()
            Diagnostics.apply(context(true))
            Diagnostics.beginLaunch("0100000000001000", a)
            val b = generations.begin()
            val begin = Diagnostics.prepareLaunch(context(true), "0100000000002000", b)
            Diagnostics.apply(context(enabled, 7, true, "guest"))
            // Models A's already accepted drain, whose lock ownership is covered above.
            Diagnostics.finishSession(a)
            assertTrue(generations.session(b, begin))
            assertEquals(enabled, nativeEnabled)
            assertEquals(enabled, Diagnostics.currentAssociation().enabled)
            assertEquals(7, applied.last().files)
            assertTrue(applied.last().verbose)
            assertEquals("guest", applied.last().categories)
        }
    }

    @Test fun aNewerPendingUserConfigurationWinsAndCompletesForTheOwnedReplacement() {
        for (enabled in listOf(false, true)) {
            Diagnostics.finishSession()
            val generations = EmulationGeneration()
            val a = generations.begin()
            Diagnostics.apply(context(true))
            Diagnostics.beginLaunch("0100000000001000", a)
            val b = generations.begin()
            val begin = Diagnostics.prepareLaunch(context(true), "0100000000002000", b)
            val entered = CountDownLatch(1)
            val release = CountDownLatch(1)
            val reader = SnapshotContext(RuntimeEnvironment.getApplication()) {
                entered.countDown()
                check(release.await(5, TimeUnit.SECONDS))
                mapOf("enabled" to enabled, "files" to 6, "categories" to "exception")
            }
            val worker = Thread { Diagnostics.apply(reader) }.apply { start() }
            try {
                assertTrue(entered.await(2, TimeUnit.SECONDS))
                Diagnostics.finishSession(a)
                assertTrue(generations.session(b, begin))
                assertFalse("Do not publish B's older enabled snapshot over a newer pending user request", nativeEnabled)
                assertFalse(Diagnostics.currentAssociation().startObserved)
            } finally { release.countDown(); worker.join(3000) }
            assertFalse(worker.isAlive)
            assertEquals(enabled, nativeEnabled)
            assertEquals(6, applied.last().files)
            assertEquals("exception", applied.last().categories)
            if (enabled) assertTrue(events.any { it.name == "SESSION_METADATA" && it.session == nativeSession &&
                it.fields["launch_start_observed"] == "false" })
        }
    }

    @Test fun anExplicitReadCannotReviveAFinishedSessionWithoutAnOwnedReplacement() {
        Diagnostics.apply(context(true))
        Diagnostics.beginLaunch("0100000000001000", 11)
        val entered = CountDownLatch(1)
        val release = CountDownLatch(1)
        val reader = SnapshotContext(RuntimeEnvironment.getApplication()) {
            entered.countDown()
            check(release.await(5, TimeUnit.SECONDS))
            mapOf("enabled" to true, "files" to 7)
        }
        val worker = Thread { Diagnostics.apply(reader) }.apply { start() }
        try {
            assertTrue(entered.await(2, TimeUnit.SECONDS))
            Diagnostics.finishSession(11)
        } finally { release.countDown(); worker.join(3000) }
        assertFalse(worker.isAlive)
        assertFalse(nativeEnabled)
        assertFalse(Diagnostics.currentAssociation().enabled)
        assertEquals(1, applied.size)
    }
}
