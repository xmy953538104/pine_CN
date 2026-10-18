// SPDX-License-Identifier: MPL-2.0
package emu.skyline.emulation

import android.app.Application
import android.os.Looper
import android.os.Process
import androidx.fragment.app.FragmentController
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.LifecycleRegistry
import androidx.savedstate.SavedStateRegistryController
import emu.skyline.EmulationActivity
import emu.skyline.diagnostics.Diagnostics
import emu.skyline.diagnostics.LaunchIdentity
import emu.skyline.settings.EmulationSettings
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference
import java.util.concurrent.CopyOnWriteArrayList
import emu.skyline.diagnostics.ProcessAssociation
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.Robolectric
import org.robolectric.RobolectricTestRunner
import org.robolectric.Shadows.shadowOf
import org.robolectric.annotation.Config
import org.robolectric.annotation.Implementation
import org.robolectric.annotation.Implements
import org.robolectric.annotation.LooperMode
import org.robolectric.shadows.ShadowProcess
import org.robolectric.util.ReflectionHelpers

/** Runs the real Activity lifecycle/completion wiring; only JNI side effects are shadowed. */
@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, manifest = Config.NONE, sdk = [29],
    shadows = [EmulationActivityCompletionTest.NativeDiagnostics::class],
    instrumentedPackages = ["emu.skyline.diagnostics.Diagnostics"])
@LooperMode(LooperMode.Mode.PAUSED)
class EmulationActivityCompletionTest {
    private data class NativeEvent(val name: String, val fields: Map<String, String>)
    @Implements(value = Diagnostics::class, isInAndroidSdk = false)
    class NativeDiagnostics {
        companion object {
            @JvmStatic @Implementation fun configureNative(path: String, enabled: Boolean, verbose: Boolean, files: Int, session: String, categories: String, process: String) = enabled
            @JvmStatic @Implementation fun statusNative() = longArrayOf(1, 0, 0, 0)
            @JvmStatic @Implementation fun setLaunchNative(id: String, title: String, newLaunch: Boolean) {
                if (newLaunch && id.isEmpty()) order.add("clear")
            }
            @JvmStatic @Implementation fun emitFieldsNative(category: String, event: String, level: String, keys: Array<String>, values: Array<String>): Boolean {
                events.add(NativeEvent(event, keys.zip(values).toMap())); order.add(event)
                return true
            }
            @JvmStatic @Implementation fun emitNative(category: String, event: String, detail: String, title: String) {
                events.add(NativeEvent(event, mapOf("detail" to detail, "title_id" to title))); order.add(event)
            }
            @JvmStatic @Implementation fun flushNative(): Boolean {
                flushes.incrementAndGet(); order.add("flush"); onFlush?.invoke(); return true
            }
            @JvmStatic @Implementation fun stopNative() {
                order.add("drain")
                drains.incrementAndGet()
                onDrain?.invoke()
            }
            @JvmStatic @Implementation fun observeHostExitNative(source: String) {
                events.add(NativeEvent("LAUNCH_HOST_EXIT_REQUESTED", mapOf("request_source" to source)))
                order.add("LAUNCH_HOST_EXIT_REQUESTED")
            }
        }
    }

    companion object {
        private val stops = AtomicInteger()
        private val drains = AtomicInteger()
        private val flushes = AtomicInteger()
        private val events = CopyOnWriteArrayList<NativeEvent>()
        private val order = CopyOnWriteArrayList<String>()
        @Volatile private var onFlush: (() -> Unit)? = null
        @Volatile private var onStop: (() -> Unit)? = null
        @Volatile private var onDrain: (() -> Unit)? = null
    }

    private lateinit var activity: EmulationActivity
    private lateinit var owner: EmulationGeneration
    private var token = 0L
    private val releaseDrain = CountDownLatch(1)
    private var originalExceptionHandler: Thread.UncaughtExceptionHandler? = null

    @Before fun attachActualActivityWithoutStartingNativeGame() {
        stops.set(0); drains.set(0); flushes.set(0); events.clear(); order.clear()
        onStop = null; onDrain = null; onFlush = null
        originalExceptionHandler = Thread.getDefaultUncaughtExceptionHandler()
        ShadowProcess.clearKilledProcesses()
        // Deliberately attach without onCreate: it starts a ROM and JNI emulation.
        // The actual onDestroy and public completion entry below remain untouched.
        activity = Robolectric.buildActivity(EmulationActivity::class.java).get()
        // FragmentActivity normally attaches its host on context availability in
        // onCreate. Initialize only that AndroidX prerequisite, not game setup.
        ReflectionHelpers.getField<SavedStateRegistryController>(activity, "mSavedStateRegistryController").performRestore(null)
        ReflectionHelpers.getField<FragmentController>(activity, "mFragments").attachHost(null)
        ReflectionHelpers.getField<LifecycleRegistry>(activity, "mFragmentLifecycleRegistry")
            .handleLifecycleEvent(Lifecycle.Event.ON_CREATE)
        activity.stopEmulationForDestruction = { join ->
            assertFalse("onDestroy must request a non-blocking native stop", join)
            stops.incrementAndGet()
            onStop?.invoke()
            true
        }
        owner = ReflectionHelpers.getStaticField(EmulationActivity::class.java, "generations")
        token = owner.begin()
        ReflectionHelpers.setField(Diagnostics, "active", false)
        ReflectionHelpers.setField(Diagnostics, "signature", "")
        Diagnostics.beginLaunch(null, token)
        ReflectionHelpers.setField(activity, "generationToken", token)
        ReflectionHelpers.setField(activity, "returnToMain", true)
        val constructor = EmulationSettings::class.java.getDeclaredConstructor(android.content.Context::class.java, String::class.java)
        constructor.isAccessible = true
        ReflectionHelpers.setField(activity, "emulationSettings", constructor.newInstance(activity, null))
    }

    @After fun releaseBlockedDrainAndInvalidateOwnership() {
        releaseDrain.countDown()
        if (::owner.isInitialized) {
            val next = owner.begin()
            owner.session(next) {} // Wait for any released drain before resetting its counters.
        }
        shadowOf(Looper.getMainLooper()).idle()
        onStop = null; onDrain = null; onFlush = null
        Thread.setDefaultUncaughtExceptionHandler(originalExceptionHandler)
    }

    private fun destroy() {
        ReflectionHelpers.callInstanceMethod<Unit>(activity, "onDestroy")
        assertEquals("actual FragmentActivity superclass destruction ran", Lifecycle.State.DESTROYED,
            ReflectionHelpers.getField<LifecycleRegistry>(activity, "mFragmentLifecycleRegistry").currentState)
    }

    private fun awaitCheckpoint(latch: CountDownLatch) {
        assertTrue("real completion drain reached checkpoint", latch.await(5, TimeUnit.SECONDS))
    }

    private fun awaitMainCondition(condition: () -> Boolean) {
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5)
        do {
            shadowOf(Looper.getMainLooper()).idle()
            if (condition()) return
            Thread.sleep(1)
        } while (System.nanoTime() < deadline)
        assertTrue("real Activity completion condition", condition())
    }

    private fun assertNoNewFinish() {
        shadowOf(Looper.getMainLooper()).idle()
        // Acceptance runs undispatched on the real main-thread entry. Check it
        // directly so an IO dispatcher delay cannot disguise a wrongly owned exit.
        assertFalse("destroyed Activity must not accept a new finish", ReflectionHelpers.getField(owner, "ending"))
        assertEquals("destroyed Activity must not start a diagnostic drain", 0, drains.get())
        assertFalse("destroyed Activity must not kill the process", ShadowProcess.wasKilled(Process.myPid()))
        assertNull("destroyed Activity must not navigate", shadowOf(activity).nextStartedActivity)
    }

    private fun startObservedLaunch(owner: Long): String {
        Diagnostics.preferences(activity).edit().putBoolean("enabled", true).commit()
        Diagnostics.apply(activity)
        Diagnostics.beginLaunch("0100751007ADA000", owner)
        Diagnostics.onNativeContentIdentity(owner, "0100751007ADA000", "cnmt_application")
        return ReflectionHelpers.getField<LaunchIdentity>(Diagnostics, "launch").id.also { assertTrue(it.isNotEmpty()) }
    }
    /** Real JVM uncaught dispatch invokes the installed production handler and its predecessor. */
    private fun runToUncaught(expected: Throwable, execute: () -> Unit): ProcessAssociation {
        val forwarded = AtomicReference<Throwable>()
        val association = AtomicReference<ProcessAssociation>()
        val finished = CountDownLatch(1)
        Thread.setDefaultUncaughtExceptionHandler { _, error ->
            forwarded.set(error); association.set(Diagnostics.currentAssociation()); finished.countDown()
        }
        Diagnostics.installExceptionObserver()
        val thread = Thread({ activity.runNativeForGeneration(token, execute) }, "ControlledEmuMain")
        thread.start(); thread.join(5000)
        assertFalse("production wrapper and handler must complete", thread.isAlive)
        assertTrue("original uncaught handler was delegated to", finished.await(1, TimeUnit.SECONDS))
        assertSame("same Throwable, without wrapping or replacement", expected, forwarded.get())
        return association.get()
    }
    @Test fun throwingProductionWrapperFlushesIdentityBeforeCleanupAndSameUncaughtHandlerKeepsIt() {
        val launchId = startObservedLaunch(token)
        val session = Diagnostics.currentAssociation().session
        val expected = LinkageError("controlled JNI boundary failure")
        // Keep controlled frames stable across Robolectric's own reflection-stack cleanup.
        expected.stackTrace = arrayOf(StackTraceElement("controlled.NativeSettings", "create", "Controlled.kt", 42),
            StackTraceElement("controlled.EmuMain", "run", "Controlled.kt", 73))
        val originalStack = expected.stackTrace.toList()
        events.clear(); order.clear(); flushes.set(0)
        val atHandler = runToUncaught(expected) { throw expected }
        val uncaught = events.single { it.name == "ANDROID_UNCAUGHT_EXCEPTION" }.fields
        assertEquals(launchId, uncaught["failure_launch_id"])
        assertEquals("0100751007ADA000", uncaught["failure_application_title_id"])
        assertEquals(session, uncaught["failure_session_id"])
        assertEquals("cnmt_application", uncaught["failure_application_identity_source"])
        assertEquals("observed_owned_launch", uncaught["failure_context_status"])
        assertEquals(originalStack, expected.stackTrace.toList())
        val before = events.single { it.name == "LAUNCH_FAILURE_CONTEXT" }.fields
        assertEquals(before["failure_id"], uncaught["failure_id"]); assertFalse(uncaught["failure_id"].isNullOrEmpty())
        assertTrue(order.indexOf("LAUNCH_FAILURE_CONTEXT") < order.indexOf("flush"))
        assertTrue(order.indexOf("flush") < order.indexOf("clear"))
        assertTrue(order.indexOf("clear") < order.indexOf("ANDROID_UNCAUGHT_EXCEPTION"))
        assertEquals(2, flushes.get()); assertEquals("", atHandler.launch); assertEquals("", atHandler.title)
        Diagnostics.preferences(activity).edit().putBoolean("verbose", true).commit()
        Diagnostics.apply(activity)
        assertEquals("", Diagnostics.currentAssociation().launch); assertEquals("", Diagnostics.currentAssociation().title)
        assertFalse(ReflectionHelpers.getField<LaunchIdentity>(Diagnostics, "launch").active)
    }
    @Test fun lateAFailureKeepsItsNativeResolvedIdentityWithoutOverwritingReplacementB() {
        val aId = startObservedLaunch(token)
        Diagnostics.beginLaunch(null, token) // A's Program identity is not known until inside execute().
        val lateAId = ReflectionHelpers.getField<LaunchIdentity>(Diagnostics, "launch").id
        assertNotEquals(aId, lateAId)
        val expected = IllegalStateException("controlled late A failure")
        var bId = ""
        var bToken = 0L
        events.clear(); order.clear()
        val atHandler = runToUncaught(expected) {
            Diagnostics.onNativeContentIdentity(token, "0100751007ADA001", "program_header")
            bToken = owner.begin()
            bId = startObservedLaunch(bToken)
            Diagnostics.onNativeContentIdentity(bToken, "0200751007ADA000", "cnmt_application")
            throw expected
        }
        val uncaught = events.single { it.name == "ANDROID_UNCAUGHT_EXCEPTION" }.fields
        assertEquals(lateAId, uncaught["failure_launch_id"])
        assertEquals("0100751007ADA001", uncaught["failure_application_title_id"])
        assertEquals("program_header", uncaught["failure_application_identity_source"])
        assertEquals(bId, atHandler.launch); assertEquals("0200751007ADA000", atHandler.title)
        val state = ReflectionHelpers.getField<LaunchIdentity>(Diagnostics, "launch")
        assertTrue(state.active); assertEquals(bToken, state.owner); assertEquals(bId, state.id)
        assertFalse("A's failure/finally never clears B", order.contains("clear"))
        assertFalse(ShadowProcess.wasKilled(Process.myPid()))
    }
    @Test fun failureBeforeNativeResolutionRetainsOnlyExplicitUnverifiedUiHint() {
        startObservedLaunch(token)
        Diagnostics.beginLaunch("0100751007ADA000", token)
        val expected = IllegalStateException("controlled NativeSettings construction failure")
        runToUncaught(expected) { throw expected }
        val fields = events.single { it.name == "ANDROID_UNCAUGHT_EXCEPTION" }.fields
        assertEquals("", fields["failure_application_title_id"])
        assertEquals("unknown", fields["failure_application_identity_source"])
        assertEquals("0100751007ADA000", fields["failure_ui_application_title_id"])
        assertEquals("ui_metadata_hint_not_runtime_verified", fields["failure_ui_identity_source"])
        assertEquals("", Diagnostics.currentAssociation().title)
    }
    @Test fun diagnosticFlushFailureCannotReplaceTheThrownObjectOrSkipOwnedCleanup() {
        val launchId = startObservedLaunch(token)
        val expected = IllegalArgumentException("controlled construction failure")
        onFlush = { throw UnsupportedOperationException("controlled sink failure") }
        val state = runToUncaught(expected) { throw expected }
        assertEquals(launchId, events.single { it.name == "ANDROID_UNCAUGHT_EXCEPTION" }.fields["failure_launch_id"])
        assertEquals("", state.launch); assertEquals("", state.title)
        assertFalse(ReflectionHelpers.getField<LaunchIdentity>(Diagnostics, "launch").active)
    }
    @Test fun destructionThenRealNativeReturnPathAndSettingsApplyCannotResurrectLaunch() {
        val old = startObservedLaunch(token)
        destroy()
        activity.runNativeForGeneration(token) { /* JNI has returned on the actual production finally path. */ }
        assertNoNewFinish()
        Diagnostics.preferences(activity).edit().putBoolean("verbose", true).commit()
        Diagnostics.apply(activity)
        val state = ReflectionHelpers.getField<LaunchIdentity>(Diagnostics, "launch")
        assertFalse(state.active); assertEquals("", state.id); assertNotEquals(old, state.id)
        assertEquals("", ReflectionHelpers.getField<String>(Diagnostics, "applicationTitleId"))
    }
    @Test fun oldNativeReturnCannotEndAReplacementLaunchOrClearItsTitle() {
        startObservedLaunch(token)
        destroy()
        val replacement = owner.begin()
        val replacementId = startObservedLaunch(replacement)
        activity.runNativeForGeneration(token) { }
        val state = ReflectionHelpers.getField<LaunchIdentity>(Diagnostics, "launch")
        assertTrue(state.active); assertEquals(replacementId, state.id); assertEquals(replacement, state.owner)
        assertEquals("0100751007ADA000", ReflectionHelpers.getField<String>(Diagnostics, "applicationTitleId"))
        assertFalse(ShadowProcess.wasKilled(Process.myPid()))
    }

    @Test fun actualMenuConfirmationRecordsRequestBeforeOwnedDrainAndKill() {
        startObservedLaunch(token); events.clear(); order.clear()
        val entered = CountDownLatch(1)
        onDrain = {
            assertEquals("menu_exit_confirmed", events.single { it.name == "LAUNCH_HOST_EXIT_REQUESTED" }.fields["request_source"])
            assertTrue(order.indexOf("LAUNCH_HOST_EXIT_REQUESTED") < order.indexOf("drain"))
            assertFalse(ShadowProcess.wasKilled(Process.myPid()))
            entered.countDown(); awaitCheckpoint(releaseDrain)
        }
        activity.confirmExitFromMenu(token)
        awaitCheckpoint(entered); releaseDrain.countDown()
        awaitMainCondition { ShadowProcess.wasKilled(Process.myPid()) }
        assertEquals(1, drains.get())
        assertFalse(events.any { it.name == "LAUNCH_ENDED" || it.name == "EMULATION_RETURNED" })
    }

    @Test fun staleMenuConfirmationCannotRecordIntoReplacementLaunch() {
        startObservedLaunch(token)
        val replacement = owner.begin(); val replacementId = startObservedLaunch(replacement)
        events.clear(); order.clear()
        activity.confirmExitFromMenu(token); shadowOf(Looper.getMainLooper()).idle()
        assertFalse(events.any { it.name == "LAUNCH_HOST_EXIT_REQUESTED" || it.name == "HOST_EXIT_REQUEST_ACCEPTED" })
        assertEquals(replacementId, Diagnostics.currentAssociation().launch)
        assertEquals(0, drains.get()); assertFalse(ShadowProcess.wasKilled(Process.myPid()))
    }

    @Test fun destroyedGenerationCannotRecordANewMenuExitRequest() {
        startObservedLaunch(token); destroy(); events.clear(); order.clear()
        activity.confirmExitFromMenu(token); shadowOf(Looper.getMainLooper()).idle()
        assertFalse(events.any { it.name == "LAUNCH_HOST_EXIT_REQUESTED" || it.name == "HOST_EXIT_REQUEST_ACCEPTED" })
        assertEquals(0, drains.get()); assertFalse(ShadowProcess.wasKilled(Process.myPid()))
    }

    @Test fun destructionBeforeNativeCompletionDoesNotStartNewExit() {
        destroy()
        assertEquals(1, stops.get())
        activity.returnFromEmulation()
        assertNoNewFinish()
    }

    @Test fun nativeStopCannotSynchronouslyAcceptCompletionDuringDestroy() {
        onStop = { activity.returnFromEmulation() }
        destroy()
        assertEquals(1, stops.get())
        assertNoNewFinish()
    }

    @Test fun queuedNativeCompletionBeforeDestroyCannotBeAcceptedAfterDestroy() {
        val nativeThread = Thread { activity.returnFromEmulation() }
        nativeThread.start(); nativeThread.join(5000)
        assertFalse("native callback posted without waiting for the UI", nativeThread.isAlive)
        assertFalse("queued completion has not yet been accepted", ReflectionHelpers.getField(owner, "ending"))
        destroy()
        assertEquals(1, stops.get())
        assertNoNewFinish()
    }

    @Test fun acceptedCompletionBeforeDestroyKeepsOwnedDrainNavigationAndExit() {
        val entered = CountDownLatch(1)
        onDrain = { entered.countDown(); awaitCheckpoint(releaseDrain) }
        activity.returnFromEmulation()
        awaitCheckpoint(entered)
        destroy()
        assertEquals(1, stops.get())
        releaseDrain.countDown()
        awaitMainCondition { ShadowProcess.wasKilled(Process.myPid()) }
        assertEquals(1, drains.get())
        assertEquals("emu.skyline.MainActivity", shadowOf(activity).nextStartedActivity?.component?.className)
    }

    @Test fun destroyingOldActivityDoesNotStopOrSuppressReplacementActivity() {
        val replacementToken = owner.begin()
        val replacement = Robolectric.buildActivity(EmulationActivity::class.java).get()
        ReflectionHelpers.setField(replacement, "generationToken", replacementToken)
        ReflectionHelpers.setField(replacement, "returnToMain", true)
        destroy()
        assertEquals("stale destruction must not stop the replacement's native game", 0, stops.get())
        assertTrue(owner.isCurrent(replacementToken))
        assertTrue(owner.session(replacementToken) { Diagnostics.beginLaunch(null, replacementToken) })
        assertNull("old Activity has not navigated before B completes", shadowOf(activity).nextStartedActivity)
        replacement.returnFromEmulation()
        awaitMainCondition { ShadowProcess.wasKilled(Process.myPid()) }
        assertEquals(1, drains.get())
        assertEquals("emu.skyline.MainActivity", shadowOf(replacement).nextStartedActivity?.component?.className)
        assertNull("exactly one navigation", shadowOf(replacement).nextStartedActivity)
    }

    @Test fun replacementStartedBySuperclassDestroyCallbackIsNotStopped() {
        var replacementToken = 0L
        ReflectionHelpers.getField<LifecycleRegistry>(activity, "mFragmentLifecycleRegistry")
            .addObserver(LifecycleEventObserver { _, event ->
                if (event == Lifecycle.Event.ON_DESTROY) replacementToken = owner.begin()
            })
        destroy()
        assertNotEquals("the real superclass destruction callback started B", 0L, replacementToken)
        assertEquals("ownership must be checked again immediately before JNI stop", 0, stops.get())
        assertTrue(owner.isCurrent(replacementToken))
        assertTrue(owner.session(replacementToken) {})
        assertEquals(0, drains.get())
        assertFalse(ShadowProcess.wasKilled(Process.myPid()))
        assertNull(shadowOf(activity).nextStartedActivity)
    }
}
