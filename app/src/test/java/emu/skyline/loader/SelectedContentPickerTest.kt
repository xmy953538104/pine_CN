// SPDX-License-Identifier: MPL-2.0
package emu.skyline.loader

import android.app.Application
import android.content.pm.ProviderInfo
import android.net.Uri
import android.os.Bundle
import android.os.Looper
import android.os.ParcelFileDescriptor
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import androidx.appcompat.app.AlertDialog
import androidx.fragment.app.Fragment
import androidx.fragment.app.FragmentActivity
import emu.skyline.R
import emu.skyline.emulation.SelectedContentLaunchTest
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.Robolectric
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.Shadows.shadowOf
import org.robolectric.annotation.Config
import org.robolectric.annotation.Implementation
import org.robolectric.annotation.Implements
import org.robolectric.shadows.ShadowContentResolver
import org.robolectric.shadows.ShadowDialog
import java.nio.file.Files
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/** UI lifecycle tests use an explicitly substituted native preview return. */
@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, manifest = Config.NONE, sdk = [29],
    instrumentedPackages = ["emu.skyline.loader"],
    shadows = [SelectedContentPickerTest.NativePreviewShadow::class, SelectedContentLaunchTest.PreviewDescriptorShadow::class])
class SelectedContentPickerTest {
    @Implements(SelectedContentBridge::class, isInAndroidSdk = false)
    class NativePreviewShadow {
        companion object {
            var inspect: (Int, String, String) -> String = { _, _, _ -> error("No preview expected") }
            @JvmStatic @Implementation fun inspect(romFd: Int, privateFilesPath: String, profile: String) =
                inspect.invoke(romFd, privateFilesPath, profile)
        }
    }

    class HostFragment : Fragment() {
        override fun onCreateView(inflater: LayoutInflater, container: ViewGroup?, state: Bundle?): View = FrameLayout(requireContext())
    }

    private fun latest() = ShadowDialog.getLatestDialog() as AlertDialog
    private fun closed(fd: ParcelFileDescriptor) = runCatching { fd.statSize < 0 }.getOrDefault(true)
    private fun drainUntil(done: () -> Boolean) {
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5)
        while (!done() && System.nanoTime() < deadline) {
            shadowOf(Looper.getMainLooper()).idle()
            Thread.yield()
        }
        assertTrue("bounded main-thread completion", done())
    }

    @Test fun profileHasNoAutomaticSelectionAndCancellationReleasesOnce() {
        val controller = Robolectric.buildActivity(FragmentActivity::class.java)
        controller.get().setTheme(R.style.AppTheme)
        controller.setup()
        val fragment = HostFragment()
        controller.get().supportFragmentManager.beginTransaction().add(android.R.id.content, fragment).commitNow()
        var finished = 0; var launched = 0
        NativePreviewShadow.inspect = { _, _, _ -> fail("No profile was chosen"); "" }
        val picker = SelectedContentPicker(fragment, Uri.parse("content://selected-synthetic/container"), { launched++ }, { finished++ })
        picker.show()
        assertEquals(2, latest().listView.adapter.count)
        assertEquals(0, finished); assertEquals(0, launched)
        latest().cancel()
        drainUntil { finished == 1 }
        assertEquals(1, finished); assertEquals(0, launched)
        controller.pause().stop().destroy()
        assertEquals(1, finished)
    }

    @Test fun cancelSuppressesLateResultButKeepsFdUntilSynchronousInspectionReturns() = pendingScan(false)

    @Test fun destroyedViewSuppressesLateResultAndClosesFdAfterSynchronousInspectionReturns() = pendingScan(true)

    private fun pendingScan(destroyView: Boolean) {
        val file = Files.createTempFile("selected-picker", ".bin").toFile()
        val controller = Robolectric.buildActivity(FragmentActivity::class.java)
        controller.get().setTheme(R.style.AppTheme)
        controller.setup()
        val fragment = HostFragment()
        controller.get().supportFragmentManager.beginTransaction().add(android.R.id.content, fragment).commitNow()
        val entered = CountDownLatch(1); val returnPreview = CountDownLatch(1)
        var finished = 0; var launched = 0
        val provider = SelectedContentLaunchTest.PreviewProvider().apply {
            this.file = file
            attachInfo(RuntimeEnvironment.getApplication(), ProviderInfo().apply { authority = "selected-synthetic" })
            ShadowContentResolver.registerProviderInternal("selected-synthetic", this)
        }
        NativePreviewShadow.inspect = { fd, _, profile ->
            assertEquals(314, fd); assertEquals("modern-40", profile)
            entered.countDown()
            check(returnPreview.await(5, TimeUnit.SECONDS)) { "synthetic_preview_deadline" }
            SelectedPreviewFixture.text()
        }
        try {
            SelectedContentPicker(fragment, Uri.parse("content://selected-synthetic/container"), { launched++ }, { finished++ }).show()
            latest().listView.performItemClick(null, 0, 0)
            shadowOf(Looper.getMainLooper()).idle()
            assertTrue(entered.await(5, TimeUnit.SECONDS))
            assertEquals(1, provider.opened.size); assertFalse(closed(provider.opened.single()))
            if (destroyView) controller.get().supportFragmentManager.beginTransaction().remove(fragment).commitNow()
            else latest().cancel()
            assertEquals(0, finished); assertEquals(0, launched)
            assertFalse("cancellation must not prematurely close a borrowed FD", closed(provider.opened.single()))
            returnPreview.countDown()
            drainUntil { finished == 1 }
            assertTrue(provider.opened.all(::closed)); assertEquals(0, launched)
            assertFalse(latest().isShowing)
        } finally {
            returnPreview.countDown()
            controller.pause().stop().destroy()
            file.delete()
        }
    }
}
