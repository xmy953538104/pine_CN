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
import emu.skyline.loader.AppEntry
import emu.skyline.loader.LoaderResult
import emu.skyline.loader.RomFormat
import emu.skyline.loader.RomType
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.Robolectric
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import org.robolectric.shadows.ShadowContentResolver
import org.robolectric.shadows.ShadowProcess
import org.robolectric.util.ReflectionHelpers
import java.io.File
import java.io.FileNotFoundException
import java.nio.file.Files

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, manifest = Config.NONE, sdk = [29])
class ContentFilesTest {
    private val base = Uri.parse("content://synthetic/base")
    private val dlcs = listOf(Uri.parse("content://synthetic/dlc-first"), Uri.parse("content://synthetic/dlc-later"))
    private val update = Uri.parse("content://synthetic/update")
    // statSize observes the actual backing RandomAccessFile, without reflective
    // access to java.io.FileDescriptor.fd blocked by JDK17's module boundaries.
    private fun closed(descriptor: ParcelFileDescriptor) = runCatching { descriptor.statSize < 0 }.getOrDefault(true)
    private fun sandbox(test: (File) -> Unit) {
        val file = Files.createTempFile("synthetic-content", ".bin").toFile()
        try { test(file) } finally { file.delete() }
    }
    private fun descriptor(file: File) = ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY)

    class MissingProvider : ContentProvider() {
        override fun onCreate() = true
        override fun openFile(uri: Uri, mode: String): ParcelFileDescriptor? =
            throw IllegalArgumentException("synthetic provider wrapper", FileNotFoundException("synthetic missing content"))
        override fun getType(uri: Uri): String? = null
        override fun query(uri: Uri, projection: Array<out String>?, selection: String?, selectionArgs: Array<out String>?, sortOrder: String?): Cursor? = null
        override fun insert(uri: Uri, values: ContentValues?): Uri? = null
        override fun delete(uri: Uri, selection: String?, selectionArgs: Array<out String>?) = 0
        override fun update(uri: Uri, values: ContentValues?, selection: String?, selectionArgs: Array<out String>?) = 0
    }

    @Test fun actualContentResolverProviderWrapperBecomesSafeFailure() {
        val context = RuntimeEnvironment.getApplication()
        val provider = MissingProvider().apply { attachInfo(context, ProviderInfo().apply { authority = "synthetic" }) }
        ShadowContentResolver.registerProviderInternal("synthetic", provider)
        try {
            openContentFiles(base, dlcs, update) { context.contentResolver.openFileDescriptor(it, "r") }
            fail("An unavailable provider must not return descriptors for native launch")
        } catch (error: ContentOpenFailure) {
            assertEquals("base", error.source)
            assertEquals("file_not_found_or_unavailable", error.reason)
            assertTrue(error.original is IllegalArgumentException)
            assertTrue(error.original.cause is FileNotFoundException)
        }
    }

    @Test fun actualActivityRejectsMissingProviderWithoutNativeLaunchOrProcessKill() {
        val context = RuntimeEnvironment.getApplication()
        val provider = MissingProvider().apply { attachInfo(context, ProviderInfo().apply { authority = "synthetic" }) }
        ShadowContentResolver.registerProviderInternal("synthetic", provider)
        val activity = Robolectric.buildActivity(EmulationActivity::class.java).get()
        val entry = AppEntry(name = "synthetic", version = "1", titleId = "0100000000001000",
            addOnContentBaseId = null, author = null, icon = null, romType = RomType.Base,
            parentTitleId = null, format = RomFormat.NSP, uri = base, loaderResult = LoaderResult.Success)
        ReflectionHelpers.setField(activity, "item", AppItem(entry, emptyList(), emptyList()))
        ShadowProcess.clearKilledProcesses()
        // Use the real private launch entry and real ContentResolver/provider.
        // No JNI replacement is installed: reaching native would fail this test.
        ReflectionHelpers.callInstanceMethod<Unit>(activity, "executeApplication",
            ReflectionHelpers.ClassParameter.from(Intent::class.java, Intent()))
        assertTrue(activity.isFinishing)
        assertFalse(ShadowProcess.wasKilled(Process.myPid()))
    }

    @Test fun dlcFailureClosesBaseAndEarlierDlcBeforeAnyUpdateOpen() = sandbox { file ->
        val opened = mutableListOf<ParcelFileDescriptor>()
        val calls = mutableListOf<Uri>()
        try {
            openContentFiles(base, dlcs, update) { uri ->
                calls.add(uri)
                if (uri == dlcs[1]) throw IllegalArgumentException("wrapper", FileNotFoundException("synthetic"))
                descriptor(file).also { opened.add(it) }
            }
            fail("Expected DLC failure")
        } catch (error: ContentOpenFailure) {
            assertEquals("dlc", error.source); assertEquals(1, error.index)
            assertEquals(listOf(base, dlcs[0], dlcs[1]), calls)
            assertEquals(2, opened.size); assertTrue(opened.all(::closed))
        }
    }

    @Test fun updateFailureClosesEveryEarlierDescriptor() = sandbox { file ->
        val opened = mutableListOf<ParcelFileDescriptor>()
        try {
            openContentFiles(base, dlcs, update) { uri ->
                if (uri == update) throw FileNotFoundException("synthetic")
                descriptor(file).also { opened.add(it) }
            }
            fail("Expected update failure")
        } catch (error: ContentOpenFailure) {
            assertEquals("update", error.source)
            assertEquals(3, opened.size); assertTrue(opened.all(::closed))
        }
    }

    @Test fun permissionAndNullProviderResultsAreActionableFailures() {
        for (permission in listOf(true, false)) {
            try {
                openContentFiles(base, emptyList(), Uri.EMPTY) {
                    if (permission) throw SecurityException("synthetic permission revoked") else null
                }
                fail("Expected safe open failure")
            } catch (error: ContentOpenFailure) {
                assertEquals(if (permission) "permission_denied" else "file_not_found_or_unavailable", error.reason)
            }
        }
    }

    @Test fun successfulOpenPreservesOriginalOrderAndDescriptorOwnership() = sandbox { file ->
        val calls = mutableListOf<Uri>()
        val opened = mutableListOf<ParcelFileDescriptor>()
        openContentFiles(base, dlcs, update) { uri ->
            calls.add(uri); descriptor(file).also { opened.add(it) }
        }.use { files ->
            assertEquals(listOf(base, dlcs[0], dlcs[1], update), calls)
            assertSame(opened[0], files.base); assertEquals(opened.subList(1, 3), files.dlcs); assertSame(opened[3], files.update)
            assertTrue(opened.none(::closed))
        }
        assertTrue(opened.all(::closed))
    }

    @Test fun unknownProgrammingFailureIsRethrownAfterClosingOwnedDescriptors() = sandbox { file ->
        val original = IllegalStateException("synthetic programming failure")
        val first = descriptor(file)
        try {
            openContentFiles(base, dlcs, update) { uri -> if (uri == base) first else throw original }
            fail("Expected original programming error")
        } catch (error: IllegalStateException) {
            assertSame(original, error); assertTrue(closed(first))
        }
    }
}
