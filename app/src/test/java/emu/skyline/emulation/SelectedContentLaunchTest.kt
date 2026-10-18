// SPDX-License-Identifier: MPL-2.0
package emu.skyline.emulation

import android.app.Application
import android.content.ContentProvider
import android.content.ContentValues
import android.content.Intent
import android.content.pm.ProviderInfo
import android.database.Cursor
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.os.Process
import emu.skyline.EmulationActivity
import emu.skyline.data.AppItem
import emu.skyline.data.AppItemTag
import emu.skyline.loader.*
import emu.skyline.settings.GameSettingsFragment
import emu.skyline.preference.GpuDriverActivity
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.Robolectric
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import org.robolectric.annotation.Implements
import org.robolectric.annotation.Implementation
import org.robolectric.shadows.ShadowContentResolver
import org.robolectric.shadows.ShadowParcelFileDescriptor
import org.robolectric.shadows.ShadowProcess
import org.robolectric.util.ReflectionHelpers
import java.io.File
import java.nio.file.Files

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, manifest = Config.NONE, sdk = [29], shadows = [SelectedContentLaunchTest.PreviewDescriptorShadow::class])
class SelectedContentLaunchTest {
    // Robolectric's getFd reflects into JDK17 java.io internals. Substitute only
    // the integer sent to the injected preview callback; open/use/close remain
    // the inherited real Robolectric PFD behavior. This is not a kernel FD test.
    @Implements(ParcelFileDescriptor::class)
    class PreviewDescriptorShadow : ShadowParcelFileDescriptor() {
        @Implementation override fun getFd() = 314
    }
    private val base = Uri.parse("content://selected-synthetic/container")
    private val dlc = Uri.parse("content://selected-synthetic/legacy-dlc")
    private val update = Uri.parse("content://selected-synthetic/legacy-update")
    private fun selection(): SelectedContentSelection {
        val catalog = SelectedPreviewFixture.catalog()
        return catalog.select(SelectedPreviewFixture.primary(catalog), null, emptyList())
    }
    private fun closed(fd: ParcelFileDescriptor) = runCatching { fd.statSize < 0 }.getOrDefault(true)
    private fun descriptor(file: File) = ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY)
    private fun rejects(block: () -> Unit) { try { block(); fail("Expected rejection") } catch (expected: IllegalArgumentException) { } }
    private fun item(uri: Uri = base, format: RomFormat = RomFormat.NSP): AppItem {
        fun entry(uri: Uri, type: RomType) = AppEntry(name = "old unrelated title", version = "old version", titleId = "0100000000009999",
            addOnContentBaseId = null, author = "old author", icon = null, romType = type, parentTitleId = "0100000000009999",
            format = format, uri = uri, loaderResult = LoaderResult.Success)
        return AppItem(entry(uri, RomType.Base), listOf(emu.skyline.data.BaseAppItem(entry(update, RomType.Update), true)),
            listOf(emu.skyline.data.BaseAppItem(entry(dlc, RomType.DLC), true)))
    }

    @Test fun absentSelectionNeverCallsAvailabilityAndMalformedPresenceNeverFallsBack() {
        assertNull(selectedLaunchSelection(false, null) { error("ordinary route must not query native") })
        var available = 0
        rejects { selectedLaunchSelection(true, "bad") { available++; true } }
        assertEquals(0, available)
        rejects { selectedLaunchSelection(true, selection().encode()) { false } }
        rejects { selectedLaunchSelection(true, null) { true } }
    }

    @Test fun explicitDispatchOnlyCallsSelectedAndDoesNotFallbackAfterFailure() {
        var ordinary = 0; var selected = 0
        dispatchContentLaunch(null, { ordinary++ }, { fail("unexpected selected route") })
        assertEquals(1, ordinary)
        val selection = selection()
        dispatchContentLaunch(selection, { fail("legacy fallback") }, { selected++; assertArrayEquals(selection.toNativeWire().fields, it.toNativeWire().fields) })
        assertEquals(1, selected)
        val failure = IllegalStateException("native validation rejected")
        try { dispatchContentLaunch(selection, { ordinary++ }, { throw failure }); fail("Expected native failure") }
        catch (error: IllegalStateException) { assertSame(failure, error) }
        assertEquals(1, ordinary)
    }

    @Test fun selectedSourceOpensOnlyOneBaseFdWithoutReadingLegacyAssociations() {
        val file = Files.createTempFile("selected-fd", ".bin").toFile()
        try {
            val calls = mutableListOf<Uri>(); val descriptors = mutableListOf<ParcelFileDescriptor>()
            openLaunchContentFiles(base, selection(), { error("legacy DLC association read") }, { error("legacy Update association read") }) {
                calls.add(it); descriptor(file).also(descriptors::add)
            }.use { files ->
                assertTrue(files.dlcs.isEmpty()); assertNull(files.update); assertFalse(closed(files.base))
            }
            assertEquals(listOf(base), calls); assertTrue(descriptors.all(::closed))
        } finally { file.delete() }
    }

    @Test fun ordinaryPlanPreservesOriginalFdOrder() {
        val file = Files.createTempFile("ordinary-fd", ".bin").toFile()
        try {
            val calls = mutableListOf<Uri>()
            openLaunchContentFiles(base, null, { listOf(dlc) }, { update }) { calls.add(it); descriptor(file) }.use { }
            assertEquals(listOf(base, dlc, update), calls)
        } finally { file.delete() }
    }

    class PreviewProvider : ContentProvider() {
        lateinit var file: File
        val opened = mutableListOf<ParcelFileDescriptor>()
        var calls = 0
        override fun onCreate() = true
        override fun openFile(uri: Uri, mode: String): ParcelFileDescriptor { calls++; return ParcelFileDescriptor.open(file,
            ParcelFileDescriptor.MODE_READ_ONLY).also(opened::add) }
        override fun getType(uri: Uri): String? = null
        override fun query(uri: Uri, projection: Array<out String>?, selection: String?, selectionArgs: Array<out String>?, sortOrder: String?): Cursor? = null
        override fun insert(uri: Uri, values: ContentValues?): Uri? = null
        override fun delete(uri: Uri, selection: String?, selectionArgs: Array<out String>?) = 0
        override fun update(uri: Uri, values: ContentValues?, selection: String?, selectionArgs: Array<out String>?) = 0
    }
    private fun provider(file: File): PreviewProvider {
        val context = RuntimeEnvironment.getApplication()
        return PreviewProvider().apply {
            this.file = file; attachInfo(context, ProviderInfo().apply { authority = "selected-synthetic" })
            ShadowContentResolver.registerProviderInternal("selected-synthetic", this)
        }
    }

    @Test fun previewBorrowsDescriptorAndClosesItOnSuccessOrInspectionFailure() {
        val file = Files.createTempFile("selected-preview", ".bin").toFile()
        try {
            val provider = provider(file); val resolver = RuntimeEnvironment.getApplication().contentResolver
            val result = inspectSelectedContent(resolver, base, "synthetic-private/", SelectedContentProfile.Modern40) { _, path, profile ->
                assertEquals("synthetic-private/", path); assertEquals("modern-40", profile); assertFalse(closed(provider.opened.last()))
                SelectedPreviewFixture.text()
            }
            assertEquals(4, result.groups.size); assertTrue(provider.opened.all(::closed))
            val expected = IllegalStateException("safe_native_error")
            try { inspectSelectedContent(resolver, base, "synthetic-private/", SelectedContentProfile.Modern40) { _, _, _ -> throw expected }; fail() }
            catch (error: IllegalStateException) { assertSame(expected, error) }
            assertEquals(2, provider.calls); assertTrue(provider.opened.all(::closed))
        } finally { file.delete() }
    }

    @Test fun activityRejectsMalformedOrUnavailableSelectionBeforeAnyFdOrNativeCall() {
        val file = Files.createTempFile("selected-never-open", ".bin").toFile()
        try {
            val provider = provider(file)
            for (payload in listOf("malformed", selection().encode())) {
                val activity = Robolectric.buildActivity(EmulationActivity::class.java).get()
                ReflectionHelpers.setField(activity, "item", item())
                activity.selectedContentAvailability = { false }
                ShadowProcess.clearKilledProcesses()
                ReflectionHelpers.callInstanceMethod<Unit>(activity, "executeApplication", ReflectionHelpers.ClassParameter.from(Intent::class.java,
                    Intent().putExtra(SelectedContentIntentTag, payload)))
                assertTrue(activity.isFinishing); assertFalse(ShadowProcess.wasKilled(Process.myPid()))
            }
            assertEquals(0, provider.calls)
        } finally { file.delete() }
    }

    @Test fun selectedPopulationNeverReusesOldTitleSaveOwnerOrContentsAssociation() {
        val original = selection()
        val other = SelectedContentSelection.create(original.mode, original.profile, "0100000000002ABC",
            original.primary.copy(groupId = "cnmt-sha256:" + "e".repeat(64), programId = "0100000000002ABD"))
        for (selection in listOf(original, other)) {
            val activity = Robolectric.buildActivity(EmulationActivity::class.java).get()
            activity.selectedContentAvailability = { true }
            activity.intent = Intent().setData(base).putExtra(AppItemTag, item()).putExtra(SelectedContentIntentTag, selection.encode())
            assertTrue(ReflectionHelpers.callInstanceMethod<Boolean>(activity, "populateAppItem"))
            val selectedItem = ReflectionHelpers.getField<AppItem>(activity, "item")
            assertEquals(selection.applicationId, activity.selectedSettingsIdentity)
            assertTrue(selectedItem.title!!.contains(selection.applicationId)); assertNull(selectedItem.titleId)
            assertNull(selectedItem.author); assertNull(selectedItem.titleVersion)
            assertTrue(selectedItem.getEnabledDlcs().isEmpty()); assertNull(selectedItem.getEnabledUpdate())
            assertEquals(Uri.EMPTY, ReflectionHelpers.getField<Uri>(activity, "updateUri"))
            assertTrue(ReflectionHelpers.getField<ArrayList<Uri>>(activity, "dlcUris").isEmpty())
            val args = contentSettingsArguments(selectedItem, selection)
            val fragment = GameSettingsFragment().apply { arguments = args }
            assertEquals(selection.applicationId, fragment.settingsIdentity)
            val driver = Robolectric.buildActivity(GpuDriverActivity::class.java, Intent().putExtras(args)).get()
            assertEquals(selection.applicationId, driver.settingsIdentity)
            assertArrayEquals(selection.toNativeWire().fields, selectedSettingsSelection(args)!!.toNativeWire().fields)
        }
    }

    @Test fun ordinarySettingsIdentityRemainsTheOriginalTitleAndMalformedSettingsCannotFallback() {
        val item = item()
        val ordinary = contentSettingsArguments(item, null)
        assertNull(selectedSettingsSelection(ordinary))
        assertEquals(item.titleId, contentSettingsIdentity(item, selectedSettingsSelection(ordinary)))
        ordinary.putString(SelectedContentIntentTag, "malformed")
        rejects { selectedSettingsSelection(ordinary) }
    }

    @Test fun sameUriSelectionChangesAreRecognizedAndInvalidReplacementIsRejected() {
        val current = Intent().setData(base).putExtra(SelectedContentIntentTag, selection().encode())
        val next = Intent().setData(base).putExtra(SelectedContentIntentTag, "invalid")
        assertFalse(selectedContentIntentChanged(current, Intent(current)))
        assertTrue(selectedContentIntentChanged(current, next))
        val activity = Robolectric.buildActivity(EmulationActivity::class.java).get()
        activity.intent = current
        activity.selectedContentAvailability = { true }
        ReflectionHelpers.callInstanceMethod<Unit>(activity, "onNewIntent", ReflectionHelpers.ClassParameter.from(Intent::class.java, next))
        assertTrue(activity.isFinishing)
    }
}
