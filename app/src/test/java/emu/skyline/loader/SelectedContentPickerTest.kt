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
import emu.skyline.diagnostics.Diagnostics
import emu.skyline.diagnostics.SelectedScanReports
import emu.skyline.diagnostics.SelectedScanReportsTest
import kotlinx.serialization.json.*
import java.io.File
import org.junit.Before
import org.junit.After
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
    instrumentedPackages = ["emu.skyline.loader", "emu.skyline.diagnostics.Diagnostics"],
    shadows = [SelectedContentPickerTest.NativePreviewShadow::class, SelectedContentLaunchTest.PreviewDescriptorShadow::class, SelectedScanReportsTest.NativeDiagnostics::class])
class SelectedContentPickerTest {
    @Implements(SelectedContentBridge::class, isInAndroidSdk = false)
    class NativePreviewShadow {
        companion object {
            var inspect: (Int, String, String) -> String = { _, _, _ -> error("No preview expected") }
            @JvmStatic @Implementation fun inspectObserved(romFd: Int, privateFilesPath: String, profile: String) =
                inspect.invoke(romFd, privateFilesPath, profile)
        }
    }

    class HostFragment : Fragment() {
        override fun onCreateView(inflater: LayoutInflater, container: ViewGroup?, state: Bundle?): View = FrameLayout(requireContext())
    }

    @Before fun diagnostics() {
        val app = RuntimeEnvironment.getApplication()
        Diagnostics.finishSession(); SelectedScanReports.awaitIdle()
        File(app.filesDir, "diagnostics").deleteRecursively()
        Diagnostics.preferences(app).edit().clear().putBoolean("enabled", true).commit()
        Diagnostics.apply(app)
    }
    @After fun closeDiagnostics() { Diagnostics.finishSession(); SelectedScanReports.awaitIdle() }
    private fun persisted(): List<JsonObject> {
        assertTrue(SelectedScanReports.awaitIdle())
        val dir = File(RuntimeEnvironment.getApplication().filesDir, "diagnostics/main")
        return (0..1).flatMap { index -> val file = File(dir, "selected-scan-$index.jsonl")
            if (file.exists()) file.readLines().filter { it.isNotBlank() }.map { Json.parseToJsonElement(it).jsonObject } else emptyList() }
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

    @Test fun v2CancellationDiscardsLateKeyReportAndKeepsBorrowedFdAlive() = pendingScan(false, true)

    @Test fun v2ViewDestructionDiscardsLateKeyReportAndKeepsBorrowedFdAlive() = pendingScan(true, true)

    @Test fun allRejectedRowsRemainAttachedVisibleDisabledAndInCatalogOrder() = rejectedRows(false)

    @Test fun v2DetailsPreserveFirstPredicateRowsAndSingleTerminalWhenCancelledAfterReport() = rejectedRows(true)

    @Test fun detailsDistinguishPresentMissingMatchAndUnobservedWithoutClaimingDecryption() {
        val context = RuntimeEnvironment.getApplication()
        val missing = SelectedScanDetails.text(context, SelectedNativeScanReport.sanitize(SelectedKeyStateFixture.loadedReport()))!!
        assertTrue(missing.contains("presence present")); assertTrue(missing.contains(context.getString(R.string.selected_observation_lookup_missing)))
        assertTrue(missing.contains("4294967295")); assertTrue(missing.contains("single container")); assertTrue(missing.contains("not_performed"))
        val matched = SelectedScanDetails.text(context, SelectedNativeScanReport.sanitize(SelectedKeyStateFixture.loadedReport("matched")))!!
        assertTrue(matched.contains(context.getString(R.string.selected_observation_lookup_matched)))
        assertFalse(matched.contains(context.getString(R.string.selected_observation_lookup_missing)))
        val unknown = SelectedScanDetails.text(context, SelectedNativeScanReport.sanitize(SelectedKeyStateFixture.report()))!!
        assertTrue(unknown.contains("not_checked")); assertTrue(unknown.contains(context.getString(R.string.selected_observation_lookup_unknown)))
        assertTrue(unknown.length <= SelectedScanDetails.MaxTextChars)
        assertNull(SelectedScanDetails.text(context, SelectedNativeScanReport.sanitize(SelectedScanTestFixture.report())))
    }

    private fun rejectedRows(v2: Boolean) {
        val file = Files.createTempFile("selected-disabled-rows", ".bin").toFile()
        val controller = Robolectric.buildActivity(FragmentActivity::class.java)
        controller.get().setTheme(R.style.AppTheme); controller.setup()
        val fragment = HostFragment()
        controller.get().supportFragmentManager.beginTransaction().add(android.R.id.content, fragment).commitNow()
        val raw = SelectedPreviewFixture.json()
        val groups = raw.getValue("groups").jsonArray
        val aocs = groups.filter { it.jsonObject.getValue("type").jsonPrimitive.int == 130 }
        fun disabled(type: Int) = groups.single { it.jsonObject.getValue("type").jsonPrimitive.int == type }.jsonObject.let {
            JsonObject(it + ("cnmt_trailer_digest_observation" to JsonPrimitive("mismatch")))
        }
        val catalog = JsonObject(raw + ("groups" to JsonArray(listOf(aocs[0], disabled(129), aocs[1], disabled(128)))))
        val provider = SelectedContentLaunchTest.PreviewProvider().apply {
            this.file = file; attachInfo(RuntimeEnvironment.getApplication(), ProviderInfo().apply { authority = "selected-synthetic" })
            ShadowContentResolver.registerProviderInternal("selected-synthetic", this)
        }
        var entered = false
        NativePreviewShadow.inspect = { _, _, _ -> entered = true; SelectedScanTestFixture.envelope(catalog,
            if (v2) SelectedKeyStateFixture.loadedReport() else SelectedScanTestFixture.report()) }
        try {
            SelectedContentPicker(fragment, Uri.parse("content://selected-synthetic/container"), { fail("disabled launch") }, {}).show()
            latest().listView.performItemClick(null, 0, 0)
            drainUntil { entered && latest().listView?.adapter?.count == 2 && !latest().listView.adapter.isEnabled(0) }
            val list = latest().listView
            assertNotNull(list.parent); assertTrue(list.isShown)
            assertFalse(list.adapter.isEnabled(0)); assertFalse(list.adapter.isEnabled(1))
            assertTrue(list.adapter.getItem(0).toString().contains(disabled(129).getValue("id").jsonPrimitive.content))
            assertTrue(list.adapter.getItem(1).toString().contains(disabled(128).getValue("id").jsonPrimitive.content))
            assertTrue(provider.opened.all(::closed))
            val report = persisted().single().getValue("fields").jsonObject
            assertEquals("completed", report.getValue("terminal_status").jsonPrimitive.content)
            assertEquals("primary_rows_all_rejected", report.getValue("ui_outcome").jsonPrimitive.content)
            assertEquals("selected_primary_all_rejected", report.getValue("ui_resource_name").jsonPrimitive.content)
            assertEquals(listOf(1, 3), report.getValue("primary_rows").jsonArray.map { it.jsonObject.getValue("ordinal").jsonPrimitive.int })
            assertEquals("cnmt_digest_not_verified", report.getValue("first_rejection_predicate").jsonPrimitive.content)
            if (v2) {
                val orderedLabels = (0 until list.adapter.count).map { list.adapter.getItem(it).toString() }
                latest().getButton(AlertDialog.BUTTON_NEUTRAL).performClick(); shadowOf(Looper.getMainLooper()).idle()
                assertTrue(latest().findViewById<android.widget.TextView>(android.R.id.message)!!.text.toString()
                    .contains(RuntimeEnvironment.getApplication().getString(R.string.selected_observation_lookup_missing)))
                latest().getButton(AlertDialog.BUTTON_POSITIVE).performClick(); shadowOf(Looper.getMainLooper()).idle()
                assertEquals(orderedLabels, (0 until latest().listView.adapter.count).map { latest().listView.adapter.getItem(it).toString() })
                assertNotNull(latest().listView.parent); assertTrue(latest().listView.isShown)
                assertFalse(latest().listView.adapter.isEnabled(0)); assertFalse(latest().listView.adapter.isEnabled(1))
                assertEquals(1, persisted().size)
                latest().getButton(AlertDialog.BUTTON_NEUTRAL).performClick(); shadowOf(Looper.getMainLooper()).idle()
            }
            latest().cancel(); shadowOf(Looper.getMainLooper()).idle()
            assertEquals(1, persisted().size)
            assertEquals("completed", persisted().single().getValue("fields").jsonObject.getValue("terminal_status").jsonPrimitive.content)
        } finally { controller.pause().stop().destroy(); file.delete() }
    }

    private fun pendingScan(destroyView: Boolean, v2: Boolean = false) {
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
            SelectedScanTestFixture.envelope(report = if (v2) SelectedKeyStateFixture.loadedReport() else SelectedScanTestFixture.report())
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
            val report = persisted().single().getValue("fields").jsonObject
            assertEquals("cancelled", report.getValue("terminal_status").jsonPrimitive.content)
            assertEquals(if (destroyView) "view_destroyed" else "user_cancelled", report.getValue("failure_code").jsonPrimitive.content)
            assertEquals(JsonNull, report.getValue("native_report"))
        } finally {
            returnPreview.countDown()
            controller.pause().stop().destroy()
            file.delete()
        }
    }
}
