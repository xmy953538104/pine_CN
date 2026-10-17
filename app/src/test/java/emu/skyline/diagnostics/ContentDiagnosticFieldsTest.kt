// SPDX-License-Identifier: MPL-2.0
package emu.skyline.diagnostics
import emu.skyline.loader.DiagnosticContentIdentity
import org.junit.Assert.*
import org.junit.Test
class ContentDiagnosticFieldsTest {
    private val app = "0100751007ADA000"
    private val patch = DiagnosticContentIdentity(app, "0100751007ADA001", "0100751007ADA800", app, "0100000000001111", "a".repeat(32), "655360", "cnmt_patch_parent")
    @Test fun associationKeepsTypedOwnerAndBoundedContentLists() {
        val fields = ContentDiagnosticFields.association(patch, patch.saveOwnerId, List(20) { patch }, listOf(null, patch))
        assertEquals(patch.cnmtId, fields["title_id"]); assertEquals(app, fields["application_title_id"])
        assertEquals("20", fields["update_count"]); assertEquals(8, fields["update_title_ids"]!!.split(",").size)
        assertEquals("legacy_owner_association_all_enabled_first_update", fields["policy"])
    }
    @Test fun duplicateDoesNotPretendMetadataWasSkippedOrRegistrationCompleted() {
        val known = ContentDiagnosticFields.imported(patch, patch.saveOwnerId, app, patch, patch.saveOwnerId, "1.0.10", "Update", "Update", "Success", true, true, true, true)
        assertEquals("native_metadata_scan", known["metadata_status"]); assertEquals("", known["metadata_unknown_reason"])
        assertEquals("duplicate_reported_success", known["decision"])
        val failed = ContentDiagnosticFields.imported(null, patch.saveOwnerId, null, null, app, null, "Unknown", "Update", "ParsingError", false, true, false, false)
        assertEquals("", failed["title_id"]); assertEquals("loader_result_not_success", failed["metadata_unknown_reason"])
        assertEquals("duplicate_reported_success", failed["decision"])
    }
    @Test fun mismatchesAndLegacySaveOwnerRemainExplicitWithoutChangingDecisions() {
        val eligible = ContentDiagnosticFields.imported(patch, patch.saveOwnerId, app, patch, patch.saveOwnerId, "1.0.10", "Update", "Update", "Success", true, false, true, true)
        assertEquals("eligible_for_registration", eligible["decision"])
        val rejected = ContentDiagnosticFields.imported(null, patch.saveOwnerId, app, null, app, "", "DLC", "Update", "Success", true, false, false, true)
        assertEquals("reject", rejected["decision"]); assertEquals("content_ids_not_supplied", rejected["metadata_unknown_reason"])
        assertEquals(patch.saveOwnerId, rejected["legacy_app_entry_title_id"])
    }
}
