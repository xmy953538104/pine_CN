// SPDX-License-Identifier: MPL-2.0
package emu.skyline.emulation

import android.content.ContentProvider
import android.content.ContentValues
import android.content.Intent
import android.content.pm.ProviderInfo
import android.database.Cursor
import android.database.MatrixCursor
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.os.Process
import android.provider.OpenableColumns
import emu.skyline.EmulationActivity
import emu.skyline.SkylineApplication
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.Robolectric
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import org.robolectric.annotation.Implementation
import org.robolectric.annotation.Implements
import org.robolectric.shadows.ShadowContentResolver
import org.robolectric.shadows.ShadowApplication
import org.robolectric.shadows.ShadowProcess
import org.robolectric.util.ReflectionHelpers
import java.io.FileNotFoundException

/** Real Activity.onCreate/cleanup; only the Application's ARM64 startup is omitted. */
@RunWith(RobolectricTestRunner::class)
@Config(application = SkylineApplication::class, manifest = Config.NONE, sdk = [29],
    shadows = [UriLaunchFailureTest.NativeApplicationStartup::class],
    instrumentedPackages = ["emu.skyline.SkylineApplication"])
class UriLaunchFailureTest {
    @Implements(value = SkylineApplication::class, isInAndroidSdk = false)
    class NativeApplicationStartup : ShadowApplication() {
        @Implementation fun onCreate() { /* Do not load an ARM64 library into the host JVM. */ }
    }
    class BrokenProvider(private val mode: String) : ContentProvider() {
        override fun onCreate() = true
        override fun getType(uri: Uri): String? = null
        override fun query(uri: Uri, projection: Array<out String>?, selection: String?, selectionArgs: Array<out String>?, sortOrder: String?): Cursor? {
            if (mode == "query-denied") throw SecurityException("synthetic revoked URI")
            if (mode == "query-null") return null
            if (mode == "query-empty") return MatrixCursor(arrayOf(OpenableColumns.DISPLAY_NAME))
            if (mode == "query-no-name") return MatrixCursor(arrayOf("synthetic_column")).apply { addRow(arrayOf("synthetic")) }
            if (mode == "query-null-name") return MatrixCursor(arrayOf(OpenableColumns.DISPLAY_NAME)).apply { addRow(arrayOf<Any?>(null)) }
            return MatrixCursor(arrayOf(OpenableColumns.DISPLAY_NAME)).apply { addRow(arrayOf("synthetic.nsp")) }
        }
        override fun openFile(uri: Uri, mode: String): ParcelFileDescriptor? = when (this.mode) {
            "open-denied" -> throw SecurityException("synthetic revoked URI")
            "open-null" -> null
            else -> throw IllegalArgumentException("synthetic provider wrapper", FileNotFoundException("synthetic unavailable URI"))
        }
        override fun insert(uri: Uri, values: ContentValues?): Uri? = null
        override fun delete(uri: Uri, selection: String?, selectionArgs: Array<out String>?) = 0
        override fun update(uri: Uri, values: ContentValues?, selection: String?, selectionArgs: Array<out String>?) = 0
    }
    private fun launch(mode: String, withUri: Boolean = true) {
        val context = RuntimeEnvironment.getApplication()
        val provider = BrokenProvider(mode).apply { attachInfo(context, ProviderInfo().apply { authority = "synthetic-uri" }) }
        ShadowContentResolver.registerProviderInternal("synthetic-uri", provider)
        val intent = Intent(Intent.ACTION_VIEW).apply { if (withUri) data = Uri.parse("content://synthetic-uri/unavailable") }
        ShadowProcess.clearKilledProcesses()
        val controller = Robolectric.buildActivity(EmulationActivity::class.java, intent)
        controller.create() // Real onCreate and populateAppItem; no AppItem extra.
        assertTrue(controller.get().isFinishing)
        controller.start().resume().pause().stop().destroy()
        assertFalse(ShadowProcess.wasKilled(Process.myPid()))
        // No Activity/JNI replacement: an accidental native launch or pause/
        // resume/stop on the rejected startup would fail with UnsatisfiedLinkError.
    }
    @Test fun revokedMetadataQueryReturnsSafely() = launch("query-denied")
    @Test fun missingProviderFileReturnsSafely() = launch("open-missing")
    @Test fun revokedFileOpenReturnsSafely() = launch("open-denied")
    @Test fun nullDescriptorReturnsSafely() = launch("open-null")
    @Test fun missingActionViewUriReturnsSafely() = launch("unused", false)
    @Test fun emptyProviderQueryReturnsSafely() = launch("query-empty")
    @Test fun missingDisplayNameReturnsSafely() = launch("query-no-name")
    @Test fun nullDisplayNameReturnsSafely() = launch("query-null-name")
    @Test fun nullProviderQueryReturnsSafely() = launch("query-null")

    @Test fun replacementActionViewValidatesUriBeforeLaunchingNative() {
        val context = RuntimeEnvironment.getApplication()
        val provider = BrokenProvider("query-denied").apply { attachInfo(context, ProviderInfo().apply { authority = "synthetic-uri" }) }
        ShadowContentResolver.registerProviderInternal("synthetic-uri", provider)
        val activity = Robolectric.buildActivity(EmulationActivity::class.java).get()
        activity.intent = Intent(Intent.ACTION_VIEW, Uri.parse("content://synthetic-uri/previous"))
        val next = Intent(Intent.ACTION_VIEW, Uri.parse("content://synthetic-uri/revoked"))
        ShadowProcess.clearKilledProcesses()
        ReflectionHelpers.callInstanceMethod<Unit>(activity, "onNewIntent", ReflectionHelpers.ClassParameter.from(Intent::class.java, next))
        assertTrue(activity.isFinishing)
        assertFalse(ShadowProcess.wasKilled(Process.myPid()))
    }
}
