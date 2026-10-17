// SPDX-License-Identifier: MPL-2.0
package emu.skyline.loader

import android.app.Application
import android.net.Uri
import emu.skyline.diagnostics.ContentDiagnosticFields
import emu.skyline.diagnostics.Diagnostics
import emu.skyline.diagnostics.ProcessAssociationCodec
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test
import org.junit.Assume.assumeTrue
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.annotation.internal.DoNotInstrument
import org.robolectric.util.ReflectionHelpers
import java.nio.file.Files
import java.io.File

@DoNotInstrument
internal object MetadataNativeBridge {
    @JvmStatic external fun populate(target: Any, mode: Int): Int
    @JvmStatic external fun resolveForLaunch(diagnosticsClass: Class<*>, owner: Long, mode: Int)
}

/** Real production JNI setters -> RomFile initializer -> unchanged AppEntry -> UI fields. */
@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, manifest = Config.NONE, sdk = [29])
class MetadataJniMappingTest {
    private val app = "0100751007ADA000"; private val patch = "0100751007ADA800"
    private val program = "0100751007ADA001"; private val save = "0100000000001111"
    @Before fun hostJni() {
        val path = System.getProperty("pine.diagnostics.metadataFixture")
        assumeTrue("Host JNI fixture supported on Linux/macOS", path != null)
        val copy = Files.createTempFile("pine-metadata-jni-", ".so").toFile()
        File(path!!).copyTo(copy, overwrite = true)
        System.load(copy.absolutePath); copy.deleteOnExit()
        ReflectionHelpers.setField(Diagnostics, "active", true)
        DiagnosticIdentityStore.clear()
    }
    @After fun clear() { Diagnostics.finishSession(); DiagnosticIdentityStore.clear(); ReflectionHelpers.setField(Diagnostics, "active", false) }
    private fun populate(mode: Int): AppEntry = RomFile.fromNativeMetadata(RomFormat.NSP, Uri.parse("content://controlled/metadata-$mode")) {
        MetadataNativeBridge.populate(it, mode)
    }.appEntry
    @Test fun distinctCnmtProgramSaveOwnerSurviveTheRealJniAndUiMapping() {
        val entry = populate(0)
        assertEquals(save, entry.titleId)
        assertEquals(RomType.Update, entry.romType); assertEquals(app, entry.parentTitleId)
        val id = entry.diagnosticIdentity!!
        assertEquals(patch, id.cnmtId); assertEquals(program, id.programId); assertEquals(save, id.saveOwnerId); assertEquals(app, id.applicationId)
        val fields = ContentDiagnosticFields.imported(id, entry.titleId, entry.parentTitleId, id, save, entry.version,
            "Update", "Update", "Success", true, false, true, true)
        assertEquals(patch, fields["title_id"]); assertEquals("cnmt", fields["content_title_id_source"])
        assertEquals(program, fields["program_id"]); assertEquals(save, fields["save_data_owner_id"])
        assertEquals("655360", fields["cnmt_version"]); assertTrue(fields.size <= 24)
        assertEquals(patch, ContentDiagnosticFields.association(id, save, listOf(id), emptyList())["update_title_ids"])
    }
    @Test fun nativeResolvedIdentityUpdatesTheActualKotlinProcessSummaryAndRejectsAnOldGeneration() {
        Diagnostics.beginLaunch(save, 101)
        assertEquals("", Diagnostics.currentAssociation().title)
        MetadataNativeBridge.resolveForLaunch(Diagnostics::class.java, 101, 1)
        val programSummary = ProcessAssociationCodec.decode(ProcessAssociationCodec.encode(Diagnostics.currentAssociation()))!!
        assertEquals(program, programSummary.title); assertEquals("program_header", programSummary.titleSource)
        assertNotEquals(save, programSummary.title)
        Diagnostics.beginLaunch(save, 102)
        MetadataNativeBridge.resolveForLaunch(Diagnostics::class.java, 102, 0)
        val patchSummary = ProcessAssociationCodec.decode(ProcessAssociationCodec.encode(Diagnostics.currentAssociation()))!!
        assertEquals(app, patchSummary.title); assertEquals("cnmt_patch_parent", patchSummary.titleSource)
        MetadataNativeBridge.resolveForLaunch(Diagnostics::class.java, 101, 1)
        assertEquals(patchSummary, Diagnostics.currentAssociation())
        Diagnostics.endLaunch(102)
        MetadataNativeBridge.resolveForLaunch(Diagnostics::class.java, 102, 0)
        assertEquals("", Diagnostics.currentAssociation().title)
    }
    @Test fun nativeIdentityCallbackSerializesWithLaunchReplacementOnTheSameMonitor() {
        Diagnostics.beginLaunch(null, 201)
        val entered = java.util.concurrent.CountDownLatch(1)
        val finished = java.util.concurrent.CountDownLatch(1)
        val failed = java.util.concurrent.atomic.AtomicReference<Throwable>()
        val worker = Thread({
            entered.countDown()
            try { MetadataNativeBridge.resolveForLaunch(Diagnostics::class.java, 201, 1) }
            catch (error: Throwable) { failed.set(error) }
            finally { finished.countDown() }
        }, "ControlledNativeIdentity")
        try {
            synchronized(Diagnostics) {
                worker.start()
                assertTrue(entered.await(2, java.util.concurrent.TimeUnit.SECONDS))
                val deadline = System.nanoTime() + java.util.concurrent.TimeUnit.SECONDS.toNanos(2)
                while (finished.count != 0L && worker.state != Thread.State.BLOCKED && System.nanoTime() < deadline) Thread.yield()
                assertEquals("native callback cannot complete while a launch transition owns the monitor", 1L, finished.count)
                assertEquals("callback cannot mutate protected identity before replacement", "", Diagnostics.currentAssociation().title)
                Diagnostics.beginLaunch(null, 202)
            }
        } finally { worker.join(3000) }
        assertFalse(worker.isAlive); assertNull(failed.get())
        assertEquals("old callback rejected after B transition", "", Diagnostics.currentAssociation().title)
        MetadataNativeBridge.resolveForLaunch(Diagnostics::class.java, 202, 0)
        assertEquals(app, Diagnostics.currentAssociation().title)
    }
    @Test fun legacySerializationKeepsItsWireFormatAndCachedObservationIsExplicit() {
        val entry = populate(0)
        val bytes = java.io.ByteArrayOutputStream().also { output ->
            java.io.ObjectOutputStream(output).use { it.writeObject(entry) }
        }.toByteArray()
        val cached = java.io.ObjectInputStream(java.io.ByteArrayInputStream(bytes)).use { it.readObject() as AppEntry }
        assertEquals(entry, cached); assertEquals(save, cached.titleId)
        assertEquals("cached_metadata_scan_not_verified_current", cached.diagnosticIdentity!!.observation)
        assertEquals(patch, cached.diagnosticIdentity!!.cnmtId)
        assertEquals("native_metadata_scan", entry.diagnosticIdentity!!.observation)
        DiagnosticIdentityStore.clear()
        assertNull(cached.diagnosticIdentity)
    }
    @Test fun programAndControlWithoutMetaNeverUseSharedSaveOwnerAsApplication() {
        val entry = populate(1); val id = entry.diagnosticIdentity!!
        assertEquals(save, entry.titleId); assertEquals("", id.cnmtId)
        assertEquals(program, id.applicationId); assertEquals(program, id.contentTitleId); assertEquals("program_header", id.applicationSource)
    }
    @Test fun controlOnlyAndOldEntriesRemainUnknownInsteadOfRelabelingSaveOwner() {
        val entry = populate(2); val id = entry.diagnosticIdentity!!
        assertEquals(save, entry.titleId); assertEquals(save, id.saveOwnerId); assertEquals("", id.applicationId); assertEquals("", id.contentTitleId)
        DiagnosticIdentityStore.clear()
        assertNull(entry.diagnosticIdentity)
        val fields = ContentDiagnosticFields.imported(null, entry.titleId, entry.parentTitleId, null, save, "", "Unknown", "Update", "Success", true, false, false, false)
        assertEquals("", fields["title_id"]); assertEquals("unavailable", fields["metadata_status"])
    }
}
