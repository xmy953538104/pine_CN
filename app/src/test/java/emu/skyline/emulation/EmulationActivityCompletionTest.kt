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
import emu.skyline.settings.EmulationSettings
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
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
    @Implements(value = Diagnostics::class, isInAndroidSdk = false)
    class NativeDiagnostics {
        companion object {
            @JvmStatic @Implementation fun stopNative() {
                drains.incrementAndGet()
                onDrain?.invoke()
            }
        }
    }

    companion object {
        private val stops = AtomicInteger()
        private val drains = AtomicInteger()
        @Volatile private var onStop: (() -> Unit)? = null
        @Volatile private var onDrain: (() -> Unit)? = null
    }

    private lateinit var activity: EmulationActivity
    private lateinit var owner: EmulationGeneration
    private var token = 0L
    private val releaseDrain = CountDownLatch(1)

    @Before fun attachActualActivityWithoutStartingNativeGame() {
        stops.set(0); drains.set(0); onStop = null; onDrain = null
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
        onStop = null; onDrain = null
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
        assertTrue(owner.session(replacementToken) {})
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
