// SPDX-License-Identifier: MPL-2.0
package emu.skyline.diagnostics
import org.junit.Assert.*
import org.junit.Test
class ContentDiagnosticFieldsTest {
    private val app = "0100751007ADA000"
    @Test fun associationKeepsTypedOwnerAndBoundedContentLists() {
        val fields = ContentDiagnosticFields.association(app, List(20) { "0100751007ada800" }, listOf(null, "0100751007ADB001"))
        assertEquals(app, fields["title_id"]); assertEquals(app, fields["application_title_id"])
        assertEquals("20", fields["update_count"]); assertEquals(8, fields["update_title_ids"]!!.split(",").size)
        assertEquals("all_enabled_first_update", fields["policy"])
    }
    @Test fun duplicateDoesNotPretendMetadataWasSkippedOrRegistrationCompleted() {
        val known = ContentDiagnosticFields.imported("0100751007ADA800", app, app, "1.0.10", "Update", "Update", "Success", true, true, true, true)
        assertEquals("available", known["metadata_status"]); assertEquals("", known["metadata_unknown_reason"])
        assertEquals("duplicate_reported_success", known["decision"])
        val failed = ContentDiagnosticFields.imported(null, null, app, null, "Unknown", "Update", "ParsingError", false, true, false, false)
        assertEquals("", failed["title_id"]); assertEquals("loader_result_not_success", failed["metadata_unknown_reason"])
        assertEquals("duplicate_reported_success", failed["decision"])
    }
    @Test fun mismatchesAndUnknownTitleRemainExplicitWithoutChangingDecisions() {
        val eligible = ContentDiagnosticFields.imported("0100751007ADA800", app, app, "1.0.10", "Update", "Update", "Success", true, false, true, true)
        assertEquals("eligible_for_registration", eligible["decision"])
        val rejected = ContentDiagnosticFields.imported("", app, app, "", "DLC", "Update", "Success", true, false, false, true)
        assertEquals("reject", rejected["decision"]); assertEquals("title_not_supplied", rejected["metadata_unknown_reason"])
    }
}
