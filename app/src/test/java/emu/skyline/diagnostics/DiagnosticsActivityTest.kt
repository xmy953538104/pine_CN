// SPDX-License-Identifier: MPL-2.0
package emu.skyline.diagnostics

import android.app.Activity
import android.app.Application
import android.content.ContentProvider
import android.content.ContentValues
import android.content.Context
import android.content.DialogInterface
import android.content.Intent
import android.database.Cursor
import android.net.Uri
import android.os.Looper
import android.os.ParcelFileDescriptor
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.TextView
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.widget.SwitchCompat
import androidx.lifecycle.lifecycleScope
import emu.skyline.BuildConfig
import emu.skyline.getPublicFilesDir
import java.io.File
import java.io.FileNotFoundException
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.zip.ZipInputStream
import kotlinx.coroutines.Job
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.Robolectric
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.Shadows.shadowOf
import org.robolectric.android.controller.ActivityController
import org.robolectric.annotation.Config
import org.robolectric.annotation.Implementation
import org.robolectric.annotation.Implements
import org.robolectric.annotation.LooperMode
import org.robolectric.shadows.ShadowApplication
import org.robolectric.shadows.ShadowDialog
import org.robolectric.shadows.ShadowToast

/** Real Activity, controls, exporter and result dispatch; JNI/picker/provider are controlled. */
@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [29],
    shadows = [DiagnosticsActivityTest.NativeDiagnostics::class],
    instrumentedPackages = ["emu.skyline.diagnostics.Diagnostics"])
@LooperMode(LooperMode.Mode.PAUSED)
class DiagnosticsActivityTest {
    private data class Configuration(val enabled: Boolean, val verbose: Boolean,
        val files: Int, val categories: String, val session: String)
    private class StatusGate {
        val entered = CountDownLatch(1)
        val release = CountDownLatch(1)
        @Volatile var timedOut = false
    }

    @Implements(value = Diagnostics::class, isInAndroidSdk = false)
    class NativeDiagnostics {
        companion object {
            @JvmStatic @Implementation fun configureNative(path: String, enabled: Boolean, verbose: Boolean,
                files: Int, session: String, categories: String, process: String): Boolean {
                nativeEnabled = enabled
                configurations.add(Configuration(enabled, verbose, files, categories, session))
                return enabled
            }
            @JvmStatic @Implementation fun statusNative(): LongArray {
                statusGate?.let { gate ->
                    gate.entered.countDown()
                    if (!gate.release.await(5, TimeUnit.SECONDS)) {
                        gate.timedOut = true
                        error("Synthetic status gate timed out")
                    }
                }
                return longArrayOf(if (nativeEnabled) 1 else 0, 0, 0, 0)
            }
            @JvmStatic @Implementation fun setLaunchNative(id: String, title: String, newLaunch: Boolean) = Unit
            @JvmStatic @Implementation fun emitFieldsNative(category: String, event: String, level: String,
                keys: Array<String>, values: Array<String>) = true
            @JvmStatic @Implementation fun emitNative(category: String, event: String, detail: String, title: String) = Unit
            @JvmStatic @Implementation fun stopNative() { nativeEnabled = false }
            @JvmStatic @Implementation fun flushNative() = true
        }
    }

    class SyntheticDestination : ContentProvider() {
        override fun onCreate() = true
        override fun getType(uri: Uri) = "application/zip"
        override fun query(uri: Uri, projection: Array<out String>?, selection: String?,
            selectionArgs: Array<out String>?, sortOrder: String?): Cursor? = null
        override fun insert(uri: Uri, values: ContentValues?): Uri? = null
        override fun delete(uri: Uri, selection: String?, selectionArgs: Array<out String>?) = 0
        override fun update(uri: Uri, values: ContentValues?, selection: String?, selectionArgs: Array<out String>?) = 0
        override fun openFile(uri: Uri, mode: String): ParcelFileDescriptor {
            destinationModes.add(mode)
            if (failDestination) throw FileNotFoundException("PUBLIC_SYNTHETIC_FAILURE_DO_NOT_DISPLAY")
            return ParcelFileDescriptor.open(destination,
                ParcelFileDescriptor.MODE_CREATE or ParcelFileDescriptor.MODE_TRUNCATE or ParcelFileDescriptor.MODE_WRITE_ONLY)
        }
    }

    companion object {
        private val configurations = CopyOnWriteArrayList<Configuration>()
        private val destinationModes = CopyOnWriteArrayList<String>()
        @Volatile private var nativeEnabled = false
        @Volatile private var failDestination = false
        @Volatile private var statusGate: StatusGate? = null
        private lateinit var destination: File
        private val destinationUri = Uri.parse("content://pine.synthetic.export/diagnostics.zip")
    }

    private lateinit var app: Application
    private lateinit var controller: ActivityController<DiagnosticsActivity>
    private var lastPickerRequestCode = 0
    private val activity: DiagnosticsActivity get() = controller.get()

    @Before fun startActualDiagnosticsActivity() {
        app = RuntimeEnvironment.getApplication()
        ShadowApplication.setProcessName(BuildConfig.APPLICATION_ID)
        Diagnostics.finishSession()
        app.getSharedPreferences("diagnostics", Context.MODE_PRIVATE).edit().clear().commit()
        configurations.clear(); destinationModes.clear()
        failDestination = false
        destination = File(app.cacheDir, "synthetic-export.zip")
        Robolectric.setupContentProvider(SyntheticDestination::class.java, "pine.synthetic.export")
        controller = Robolectric.buildActivity(DiagnosticsActivity::class.java).setup()
        shadowOf(Looper.getMainLooper()).idle()
    }

    @After fun stopActualDiagnosticsActivity() {
        statusGate?.release?.countDown()
        statusGate = null
        ShadowDialog.getLatestDialog()?.dismiss()
        if (::controller.isInitialized) controller.pause().stop().destroy()
        shadowOf(Looper.getMainLooper()).idle()
        Diagnostics.finishSession()
    }

    private fun views(root: View = activity.findViewById(android.R.id.content)): Sequence<View> = sequence {
        yield(root)
        if (root is ViewGroup) for (index in 0 until root.childCount) yieldAll(views(root.getChildAt(index)))
    }
    private fun button(prefix: String) = views().filterIsInstance<Button>().single { it.text.startsWith(prefix) }
    private fun toggle(prefix: String) = views().filterIsInstance<SwitchCompat>().single { it.text.startsWith(prefix) }
    private fun await(description: String, predicate: () -> Boolean) {
        val end = System.nanoTime() + TimeUnit.SECONDS.toNanos(8)
        while (System.nanoTime() < end) {
            shadowOf(Looper.getMainLooper()).idle()
            if (predicate()) return
            Thread.sleep(5)
        }
        fail("Timed out: $description")
    }
    private fun snapshots() = Diagnostics.exportDirectory(app).listFiles().orEmpty()
        .filter { it.isDirectory && it.name.startsWith("snapshot-") }
    private fun preview(): AlertDialog {
        val old = ShadowDialog.getLatestDialog()
        assertTrue(button("Preview and export").performClick())
        await("actual preview dialog") { ShadowDialog.getLatestDialog().let { it !== old && it?.isShowing == true } }
        return ShadowDialog.getLatestDialog() as AlertDialog
    }
    private fun chooseDestination(): Intent {
        preview().getButton(DialogInterface.BUTTON_POSITIVE).performClick()
        shadowOf(Looper.getMainLooper()).idle()
        val started = shadowOf(activity).nextStartedActivityForResult
        assertNotNull(started)
        assertEquals(Intent.ACTION_CREATE_DOCUMENT, started.intent.action)
        assertEquals("application/zip", started.intent.type)
        assertTrue(started.intent.getStringExtra(Intent.EXTRA_TITLE)!!.startsWith("pine-diag-"))
        lastPickerRequestCode = started.requestCode
        return started.intent
    }
    private fun latestDialog() = ShadowDialog.getLatestDialog() as AlertDialog
    private fun awaitExportFailure() = await("visible export failure") {
        ShadowToast.getTextOfLatestToast()?.startsWith("Export failed:") == true
    }
    private fun contents(): Map<String, String> {
        val contents = linkedMapOf<String, String>()
        ZipInputStream(destination.inputStream()).use { zip ->
            while (true) {
                val entry = zip.nextEntry ?: break
                check(contents.put(entry.name, zip.readBytes().toString(Charsets.UTF_8)) == null)
            }
        }
        return contents
    }
    private fun seedLogs() {
        File(Diagnostics.directory(app), "diagnostics-0.jsonl").apply {
            parentFile!!.mkdirs()
            writeText("""{"schema":1,"session_id":"synthetic-session","sequence":1,"level":"INFO","category":"guest","event":"UI_EXPORT_CANARY","fields":{"message":"hello 世界"}}
""")
        }
        File(app.getPublicFilesDir(), "logs/emulation.log").apply {
            parentFile!!.mkdirs(); writeText("content://synthetic/private/game.nsp\nhello 世界\n")
        }
    }

    @Test fun controlsStartOffAndActualToggleReconfiguresSession() {
        val enabled = toggle("Enable diagnostics")
        val detailed = toggle("Detailed tracing")
        assertFalse(enabled.isChecked); assertFalse(detailed.isChecked)
        assertFalse(app.getSharedPreferences("diagnostics", Context.MODE_PRIVATE).getBoolean("enabled", false))
        enabled.performClick()
        await("enabled native boundary") { configurations.lastOrNull()?.enabled == true }
        val first = configurations.last().session
        assertTrue(Regex("[0-9a-f-]{36}").matches(first))
        detailed.performClick()
        await("detailed native boundary") { configurations.lastOrNull()?.verbose == true }
        assertTrue(app.getSharedPreferences("diagnostics", Context.MODE_PRIVATE).getBoolean("verbose", false))
        assertNotEquals(first, configurations.last().session)
        enabled.performClick()
        await("disabled native boundary") { configurations.lastOrNull()?.enabled == false }
        assertFalse(enabled.isChecked)
    }

    @Test fun previewCancelDeletesPreparedCopiesWithoutLaunchingPicker() {
        seedLogs()
        val dialog = preview()
        val message = requireNotNull(dialog.findViewById<TextView>(android.R.id.message)).text.toString()
        assertTrue(message.contains("main/diagnostics-0.jsonl"))
        assertTrue(message.contains("session-metadata.txt"))
        assertEquals(1, snapshots().size)
        dialog.getButton(DialogInterface.BUTTON_NEGATIVE).performClick()
        await("snapshot released after preview cancel") { snapshots().isEmpty() }
        assertNull(shadowOf(activity).nextStartedActivityForResult)
        assertTrue(button("Preview and export").isEnabled)
    }

    @Test fun pickerCancelReleasesSnapshotAndAllowsAnotherPreview() {
        seedLogs()
        val launched = chooseDestination()
        assertEquals(1, snapshots().size)
        shadowOf(activity).receiveResult(launched, Activity.RESULT_CANCELED, null)
        await("snapshot released after picker cancel") { snapshots().isEmpty() }
        assertFalse(destination.exists())
        preview().getButton(DialogInterface.BUTTON_NEGATIVE).performClick()
        await("second preview cleanup") { snapshots().isEmpty() }
    }

    @Test fun pickerSuccessWritesRealAllowlistedRedactedZipAndCleansSnapshot() {
        seedLogs()
        val launched = chooseDestination()
        shadowOf(activity).receiveResult(launched, Activity.RESULT_OK, Intent().setData(destinationUri))
        await("successful exported ZIP") { ShadowToast.getTextOfLatestToast() == "Diagnostics exported" }
        assertEquals(listOf("wt"), destinationModes.toList())
        val contents = contents()
        assertEquals(setOf("session-metadata.txt", "main/diagnostics-0.jsonl", "emulation.log"), contents.keys)
        assertEquals(BuildConfig.DIAGNOSTICS_COMMIT, contents.getValue("session-metadata.txt")
            .lineSequence().single { it.startsWith("exporter_commit=") }.substringAfter('='))
        assertTrue(contents.getValue("main/diagnostics-0.jsonl").contains("UI_EXPORT_CANARY"))
        assertTrue(contents.getValue("emulation.log").contains("hello 世界"))
        assertFalse(contents.values.any { it.contains("content://synthetic") })
        assertTrue(snapshots().isEmpty())
    }

    @Test fun destinationFailureIsVisiblePrivateAndRetryCanSucceed() {
        seedLogs()
        failDestination = true
        val failed = chooseDestination()
        shadowOf(activity).receiveResult(failed, Activity.RESULT_OK, Intent().setData(destinationUri))
        awaitExportFailure()
        assertFalse(ShadowToast.getTextOfLatestToast().contains("PUBLIC_SYNTHETIC_FAILURE"))
        assertTrue(snapshots().isEmpty())
        failDestination = false
        val retry = chooseDestination()
        shadowOf(activity).receiveResult(retry, Activity.RESULT_OK, Intent().setData(destinationUri))
        await("successful retry") { ShadowToast.getTextOfLatestToast() == "Diagnostics exported" }
        assertTrue(contents().getValue("main/diagnostics-0.jsonl").contains("UI_EXPORT_CANARY"))
        assertTrue(snapshots().isEmpty())
    }

    @Test fun failedPreviewReenablesControlAndCanRecover() {
        val cache = Diagnostics.exportDirectory(app)
        assertFalse(cache.exists())
        cache.writeText("synthetic blocker: a file instead of a cache directory")
        button("Preview and export").performClick()
        await("visible preview failure") { ShadowToast.getTextOfLatestToast()?.startsWith("Preview failed:") == true }
        assertTrue(button("Preview and export").isEnabled)
        assertNull(shadowOf(activity).nextStartedActivityForResult)
        assertTrue(cache.delete())
        preview().getButton(DialogInterface.BUTTON_NEGATIVE).performClick()
        await("recovered preview cleanup") { snapshots().isEmpty() }
    }

    @Test fun retentionControlPersistsAndAppliesSelectedLimit() {
        button("Retention /").performClick()
        shadowOf(Looper.getMainLooper()).idle()
        val list = latestDialog().listView
        assertEquals(3, list.count)
        list.performItemClick(list.getChildAt(2), 2, 2L)
        await("selected retention configuration") { configurations.lastOrNull()?.files == 8 }
        assertEquals(8, app.getSharedPreferences("diagnostics", Context.MODE_PRIVATE).getInt("files", -1))
    }

    @Test fun resourceCategoryCanBeSelectedAndPersistedThroughActualDialog() {
        val prefs = app.getSharedPreferences("diagnostics", Context.MODE_PRIVATE)
        prefs.edit().putString("categories", "content").commit()
        button("Diagnostic categories").performClick()
        shadowOf(Looper.getMainLooper()).idle()
        val dialog = latestDialog()
        val list = dialog.listView
        val resource = (0 until list.count).single { list.getItemAtPosition(it) == "resource" }
        assertFalse(list.isItemChecked(resource))
        list.performItemClick(list.getChildAt(resource), resource, resource.toLong())
        dialog.getButton(DialogInterface.BUTTON_POSITIVE).performClick()
        await("resource category applied") { configurations.lastOrNull()?.categories == "content,resource" }
        assertEquals("content,resource", prefs.getString("categories", ""))
        button("Diagnostic categories").performClick()
        shadowOf(Looper.getMainLooper()).idle()
        assertTrue(latestDialog().listView.isItemChecked(resource))
        latestDialog().getButton(DialogInterface.BUTTON_NEGATIVE).performClick()
    }

    @Test fun categoryDialogCancellationDoesNotPersistAndEmptySelectionMeansNone() {
        button("Diagnostic categories").performClick()
        shadowOf(Looper.getMainLooper()).idle()
        latestDialog().listView.let { it.performItemClick(it.getChildAt(0), 0, 0L) }
        latestDialog().getButton(DialogInterface.BUTTON_NEGATIVE).performClick()
        shadowOf(Looper.getMainLooper()).idle()
        assertEquals("", app.getSharedPreferences("diagnostics", Context.MODE_PRIVATE).getString("categories", ""))
        assertTrue(configurations.isEmpty())
        button("Diagnostic categories").performClick()
        shadowOf(Looper.getMainLooper()).idle()
        val dialog = latestDialog()
        val list = dialog.listView
        val moduleRuntime = (0 until list.count).single { list.getItemAtPosition(it) == "module_runtime" }
        assertFalse("Experimental runtime identity must require explicit selection", list.isItemChecked(moduleRuntime))
        val selected = (0 until list.count).filter { list.isItemChecked(it) }
        assertEquals(setOf("content", "storage", "guest", "exception", "resource"),
            selected.map { list.getItemAtPosition(it) }.toSet())
        for (index in selected) {
            list.performItemClick(list.getChildAt(index), index, index.toLong())
        }
        assertTrue((0 until list.count).all { !list.isItemChecked(it) })
        dialog.getButton(DialogInterface.BUTTON_POSITIVE).performClick()
        await("explicit none category configuration") { configurations.lastOrNull()?.categories == "none" }
        assertEquals("none", app.getSharedPreferences("diagnostics", Context.MODE_PRIVATE).getString("categories", ""))
    }

    @Test fun pickerCancelAfterRecreationReleasesRetainedSnapshot() {
        seedLogs()
        chooseDestination()
        val code = lastPickerRequestCode
        val old = activity
        controller = controller.recreate()
        assertNotSame(old, activity)
        assertEquals(1, snapshots().size)
        assertTrue(activity.activityResultRegistry.dispatchResult(code, Activity.RESULT_CANCELED, null))
        await("retained snapshot cancellation") { snapshots().isEmpty() }
        preview().getButton(DialogInterface.BUTTON_NEGATIVE).performClick()
        await("new Activity preview cleanup") { snapshots().isEmpty() }
    }

    @Test fun pickerSuccessAfterRecreationExportsOriginalPreparedBytes() {
        seedLogs()
        chooseDestination()
        val code = lastPickerRequestCode
        val originalMetadata = File(snapshots().single(), "session-metadata.txt").readText()
        File(Diagnostics.directory(app), "diagnostics-0.jsonl").writeText("changed after preview\n")
        controller = controller.recreate()
        assertTrue(activity.activityResultRegistry.dispatchResult(code, Activity.RESULT_OK, Intent().setData(destinationUri)))
        await("successful export after recreation") { ShadowToast.getTextOfLatestToast() == "Diagnostics exported" }
        assertEquals(originalMetadata, contents().getValue("session-metadata.txt"))
        assertTrue(contents().getValue("main/diagnostics-0.jsonl").contains("UI_EXPORT_CANARY"))
        assertFalse(contents().values.any { it.contains("changed after preview") })
        assertTrue(snapshots().isEmpty())
    }

    @Test fun repeatedClickWhilePreparingProducesOnlyOnePreview() {
        await("initial status refresh") {
            views().filterIsInstance<TextView>().any { it.text.startsWith("Status process:") }
        }
        val old = ShadowDialog.getLatestDialog()
        val export = button("Preview and export")
        val owner = requireNotNull(activity.lifecycleScope.coroutineContext[Job])
        val existingJobs = owner.children.toSet()
        val gate = StatusGate()
        statusGate = gate
        var preparations = emptyList<Job>()
        try {
            export.performClick()
            shadowOf(Looper.getMainLooper()).idle()
            assertTrue("first preparation reached real IO worker", gate.entered.await(2, TimeUnit.SECONDS))
            export.performClick()
            shadowOf(Looper.getMainLooper()).idle()
            preparations = owner.children.filter { it !in existingJobs }.toList()
            // All launched jobs are registered synchronously. Holding the first IO worker
            // prevents a duplicate preparation from completing before it can be counted.
            assertEquals("only one live preparation after both clicks", 1, preparations.size)
        } finally {
            preparations = owner.children.filter { it !in existingJobs }.toList()
            gate.release.countDown()
            await("all preparation workers completed") { preparations.all { it.isCompleted } }
            statusGate = null
        }
        assertFalse("preparation barrier must not time out", gate.timedOut)
        assertTrue(ShadowDialog.getLatestDialog().let { it !== old && it?.isShowing == true })
        assertEquals(1, snapshots().size)
        latestDialog().getButton(DialogInterface.BUTTON_NEGATIVE).performClick()
        await("single preview cleanup") { snapshots().isEmpty() }
    }

    @Test fun successfulPickerResultWithoutUriIsTreatedAsCancellation() {
        val launched = chooseDestination()
        shadowOf(activity).receiveResult(launched, Activity.RESULT_OK, Intent())
        await("empty URI result cleanup") { snapshots().isEmpty() }
        assertTrue(destinationModes.isEmpty())
        assertFalse(destination.exists())
    }

    @Test fun expiredSnapshotFailsVisiblyWithoutSuccessAndAllowsFreshRetry() {
        seedLogs()
        val launched = chooseDestination()
        assertTrue(File(snapshots().single(), "session-metadata.txt").delete())
        shadowOf(activity).receiveResult(launched, Activity.RESULT_OK, Intent().setData(destinationUri))
        awaitExportFailure()
        assertTrue(snapshots().isEmpty())
        val retry = chooseDestination()
        shadowOf(activity).receiveResult(retry, Activity.RESULT_OK, Intent().setData(destinationUri))
        await("fresh export after eviction") { ShadowToast.getTextOfLatestToast() == "Diagnostics exported" }
        assertTrue(contents().containsKey("session-metadata.txt"))
        assertTrue(snapshots().isEmpty())
    }

    @Test fun settingsRecreationPreservesEnabledSessionWithoutNativeRestart() {
        toggle("Enable diagnostics").performClick()
        await("initial enabled session") { configurations.lastOrNull()?.enabled == true }
        val calls = configurations.toList()
        val session = Diagnostics.session(app)
        controller = controller.recreate()
        shadowOf(Looper.getMainLooper()).idle()
        assertTrue(toggle("Enable diagnostics").isChecked)
        assertEquals(calls, configurations.toList())
        assertEquals(session, Diagnostics.session(app))
        assertTrue(nativeEnabled)
    }
}
