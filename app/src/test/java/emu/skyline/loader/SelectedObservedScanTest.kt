// SPDX-License-Identifier: MPL-2.0
package emu.skyline.loader

import emu.skyline.diagnostics.SelectedScanRecord
import emu.skyline.diagnostics.SelectedScanSession
import kotlinx.serialization.json.*
import org.junit.Assert.*
import org.junit.Test
import java.io.File
import java.security.MessageDigest

/** Constructed report cases are separate from exact native AES report resources. */
internal object SelectedScanTestFixture {
    fun contract() = Json.parseToJsonElement(File("../tests/selected-scan/report-contract.json").readText()).jsonObject
    fun report() = contract().getValue("example_envelope").jsonObject.getValue("scan_report").jsonObject
    fun envelope(catalog: JsonObject = SelectedPreviewFixture.json(), report: JsonObject = report()): String = buildJsonObject {
        put("selected_scan_schema", 1); put("status", report.getValue("status")); put("scan_report", report)
        put("catalog", if (report.getValue("status").jsonPrimitive.content == "failed") JsonNull else catalog)
    }.toString()
    fun fields(native: JsonObject? = report(), terminal: String = "completed") = buildJsonObject {
        put("scan_attempt_id", "00000000-0000-0000-0000-000000000001"); put("profile", "modern-40")
        put("terminal_status", terminal); put("started_elapsed_us", 10); put("duration_us", 20); put("dropped_report_count", 0)
        put("ui_stage", "primary_groups"); put("ui_resource_name", "selected_primary_title"); put("ui_outcome", "primary_rows_available")
        put("failure_code", JsonNull); put("first_rejection_predicate", JsonNull); put("primary_rows", JsonArray(emptyList()))
        put("primary_rows_omitted", 0)
        put("build", buildJsonObject {
            put("application_id", "io.github.pine.synthetic"); put("version_code", 1); put("version_name", "1.0-test")
            put("build_type", "debug"); put("commit", "40419e0"); put("selected_content_enabled", true)
        })
        put("native_report", native ?: JsonNull)
    }
    fun record(sequence: Long = 1, fields: JsonObject = fields()) = SelectedScanRecord.create(
        SelectedScanSession("00000000-0000-0000-0000-000000000002"), sequence, fields)
}

class SelectedObservedScanTest {
    private fun rejected(code: String, block: () -> Unit) {
        try { block(); fail("Expected safe failure") } catch (error: SelectedScanFailure) { assertEquals(code, error.code) }
    }

    @Test fun actualNativeAesReportsPreserveBackendKeyCnmtBudgetAndDigestObservations() {
        fun bytes(name: String) = requireNotNull(javaClass.getResourceAsStream("/selected-scan/native-reports/$name")).use { it.readBytes() }
        val hashes = Json.parseToJsonElement(bytes("sha256.json").toString(Charsets.UTF_8)).jsonObject
        val reports = hashes.mapValues { (name, digest) ->
            val content = bytes(name)
            assertEquals(digest.jsonPrimitive.content, MessageDigest.getInstance("SHA-256").digest(content).joinToString("") { "%02x".format(it) })
            val original = Json.parseToJsonElement(content.toString(Charsets.UTF_8)).jsonObject
            SelectedNativeScanReport.sanitize(original).safe.also { assertEquals(original, it) }
        }
        fun summary(name: String) = reports.getValue("$name.json").getValue("catalog_summary").jsonObject
        assertEquals(listOf("missing_key_area", "meta_cnmt_unreadable"), summary("missing-meta-area-key").getValue("issue_codes").jsonArray.map { it.jsonPrimitive.content })
        assertEquals(0, summary("missing-meta-area-key").getValue("group_count").jsonPrimitive.int)
        assertFalse(summary("missing-meta-area-key").getValue("all_metadata_enumerated").jsonPrimitive.boolean)
        assertEquals("invalid", summary("malformed-cnmt").getValue("groups").jsonArray.single().jsonObject.getValue("parsing").jsonPrimitive.content)
        assertFalse(summary("native-budget").getValue("bounded_scan_complete").jsonPrimitive.boolean)
        assertEquals("scan_budget", summary("native-budget").getValue("issue_codes").jsonArray.first().jsonPrimitive.content)
        val digest = summary("digest-observation").getValue("groups").jsonArray.single().jsonObject
        assertEquals("parsed", digest.getValue("parsing").jsonPrimitive.content); assertEquals("mismatch", digest.getValue("digest_observation").jsonPrimitive.content)
        assertEquals("input_changed", reports.getValue("source-changed-serialize.json").getValue("error_code").jsonPrimitive.content)
        assertEquals("catalog_serialize", reports.getValue("source-changed-serialize.json").getValue("failed_stage").jsonPrimitive.content)
    }

    @Test fun exactNativeContractKeysAndAllowlistAreIndependentlyMatched() {
        val contract = SelectedScanTestFixture.contract()
        assertEquals(contract.getValue("code_allowlist").jsonArray.map { it.jsonPrimitive.content }.toSet(), SelectedNativeScanReport.codes)
        assertEquals(116, SelectedNativeScanReport.codes.size)
        val parsed = SelectedObservedScan.parse(SelectedScanTestFixture.envelope(), SelectedContentProfile.Modern40)
        assertEquals(4, parsed.catalog!!.groups.size)
        assertEquals("missing_header_key", parsed.report.firstIssue)
        assertEquals(SelectedScanTestFixture.report().keys, parsed.report.safe.keys)
        assertEquals("decrypted_header", Json.parseToJsonElement(SelectedPreviewFixture.text()).jsonObject.getValue("ncas").jsonArray.first().jsonObject.getValue("readability").jsonPrimitive.content)
    }

    @Test fun rawCatalogAndPrivateCanariesNeverSurviveSafeReportOrPersistedSanitization() {
        val canary = "PRIVATE_CANARY_DO_NOT_EXPORT"
        val original = SelectedScanTestFixture.report()
        val summary = original.getValue("catalog_summary").jsonObject
        val malicious = JsonObject(original + mapOf("private_path" to JsonPrimitive("/data/$canary"), "catalog" to buildJsonObject { put("catalog_kind", canary) },
            "error_code" to JsonPrimitive(canary), "catalog_summary" to JsonObject(summary + mapOf(
                "issue_codes" to JsonArray(listOf(JsonPrimitive(canary), JsonPrimitive("missing_key_area"))),
                "members" to JsonArray(listOf(buildJsonObject {
                    put("ordinal", 0); put("nca_type", JsonNull); put("readability", canary); put("support", canary)
                    put("member_id", "content://$canary/rom"); put("rights_id", canary)
                    put("issue_codes", JsonArray(listOf(JsonPrimitive(canary))))
                }))))))
        val safe = SelectedNativeScanReport.sanitize(malicious).safe
        assertFalse(safe.toString().contains(canary)); assertFalse(safe.toString().contains("catalog_kind"))
        assertEquals("other_native_issue", safe.getValue("error_code").jsonPrimitive.content)
        assertEquals("unknown", safe.getValue("catalog_summary").jsonObject.getValue("members").jsonArray.single().jsonObject.getValue("readability").jsonPrimitive.content)
        val record = SelectedScanTestFixture.record(fields = SelectedScanTestFixture.fields(safe))
        val sanitized = SelectedScanRecord.sanitize(JsonObject(record + mapOf("catalog" to SelectedPreviewFixture.json(), "private" to JsonPrimitive(canary))))!!
        assertFalse(sanitized.toString().contains(canary)); assertFalse("catalog" in sanitized)
        assertNull(SelectedScanRecord.sanitize(JsonObject(record + ("sequence" to JsonPrimitive(1)))))
    }

    @Test fun envelopeFailuresRetainOnlySafeReportAndNeverParseFailedCatalog() {
        val failure = JsonObject(SelectedScanTestFixture.report() + mapOf("status" to JsonPrimitive("failed"),
            "failed_stage" to JsonPrimitive("catalog_scan"), "error_code" to JsonPrimitive("missing_key_area")))
        val parsed = SelectedObservedScan.parse(SelectedScanTestFixture.envelope(report = failure), SelectedContentProfile.Modern40)
        assertNull(parsed.catalog); assertEquals("missing_key_area", parsed.report.safe.getValue("error_code").jsonPrimitive.content)
        val malformedFailed = Json.parseToJsonElement(SelectedScanTestFixture.envelope(report = failure)).jsonObject
        rejected("observed_report_invalid") {
            SelectedObservedScan.parse(JsonObject(malformedFailed + ("catalog" to SelectedPreviewFixture.json())).toString(), SelectedContentProfile.Modern40)
        }
        rejected("observed_json_invalid") { SelectedObservedScan.parse("{", SelectedContentProfile.Modern40) }
        rejected("observed_json_limit") { SelectedObservedScan.parse("x".repeat(SelectedObservedScan.MaxEnvelopeChars + 1), SelectedContentProfile.Modern40) }
        val oversized = JsonObject(SelectedScanTestFixture.report() + ("canary" to JsonPrimitive("x".repeat(SelectedNativeScanReport.MaxBytes))))
        rejected("observed_report_invalid") { SelectedObservedScan.parse(SelectedScanTestFixture.envelope(report = oversized), SelectedContentProfile.Modern40) }
        rejected("catalog_json_invalid") { SelectedObservedScan.parse(SelectedScanTestFixture.envelope(buildJsonObject { put("private", "never_in_error") }), SelectedContentProfile.Modern40) }
        val malformed = JsonObject(SelectedScanTestFixture.report() + ("elapsed_us" to JsonPrimitive(-1)))
        rejected("observed_report_invalid") { SelectedObservedScan.parse(SelectedScanTestFixture.envelope(report = malformed), SelectedContentProfile.Modern40) }
    }

    @Test fun emptyAocOnlyAndDisabledPatchAreDistinctWithoutChangingSelection() {
        val raw = SelectedPreviewFixture.json()
        fun catalog(groups: JsonArray) = SelectedPreviewFixture.catalog(JsonObject(raw + ("groups" to groups)))
        val empty = primaryState(catalog(JsonArray(emptyList())))
        assertEquals("no_groups", empty.outcome); assertEquals("selected_no_candidates", empty.resource)
        val aocs = raw.getValue("groups").jsonArray.filter { it.jsonObject.getValue("type").jsonPrimitive.int == 130 }
        assertEquals("no_primary_role", primaryState(catalog(JsonArray(aocs))).outcome)
        val patch = raw.getValue("groups").jsonArray.single { it.jsonObject.getValue("type").jsonPrimitive.int == 129 }.jsonObject
        val disabled = JsonObject(patch + ("cnmt_trailer_digest_observation" to JsonPrimitive("mismatch")))
        val mixed = catalog(JsonArray(listOf(aocs.first(), disabled)))
        val state = primaryState(mixed)
        assertEquals("primary_rows_all_rejected", state.outcome); assertEquals("selected_primary_all_rejected", state.resource)
        assertEquals(1, state.primaryRows.single().ordinal)
        assertEquals(SelectedRejection.InvalidMetadata, state.primaryRows.single().rejection)
        assertEquals("cnmt_digest_not_verified", state.primaryRows.single().predicate)
        assertTrue(mixed.programs(disabled.getValue("id").jsonPrimitive.content).all { it.rejection != null })
        val complete = SelectedPreviewFixture.catalog()
        val primary = SelectedPreviewFixture.primary(complete)
        val selection = complete.select(primary, null, emptyList())
        assertEquals("primary_rows_available", primaryState(complete).outcome)
        assertArrayEquals(SelectedPreviewFixture.fields(), selection.toNativeWire().fields)
        val alsoInvalid = JsonObject(disabled + ("packaged_header" to JsonObject(disabled.getValue("packaged_header").jsonObject + ("install_type" to JsonPrimitive(1)))))
        assertEquals("cnmt_digest_not_verified", primaryState(catalog(JsonArray(listOf(alsoInvalid)))).primaryRows.single().predicate)
    }

    @Test fun reportRowsIssuesAndRecordBytesStayBounded() {
        val report = SelectedScanTestFixture.report(); val summary = report.getValue("catalog_summary").jsonObject
        val group = buildJsonObject { put("ordinal", 0); put("role", "patch"); put("parsing", "parsed"); put("digest_observation", "mismatch")
            put("issue_codes", JsonArray(emptyList())); put("title_id", "/data/not-an-id"); put("application_id", "0100000000001000") }
        val many = JsonObject(report + ("catalog_summary" to JsonObject(summary + mapOf("groups" to JsonArray(List(40) { group }), "members" to JsonArray(emptyList())))))
        val safe = SelectedNativeScanReport.sanitize(many).safe.getValue("catalog_summary").jsonObject
        assertEquals(32, safe.getValue("groups").jsonArray.size); assertEquals(8, safe.getValue("groups_omitted").jsonPrimitive.int)
        assertFalse(safe.toString().contains("not-an-id")); assertTrue(safe.toString().contains("0100000000001000"))
        val record = SelectedScanTestFixture.record()
        assertTrue(record.toString().toByteArray().size + 1 <= SelectedScanRecord.MaxRecordBytes)
        assertNull(SelectedScanRecord.parseLine("x".repeat(SelectedScanRecord.MaxRecordBytes + 1)))
    }
}
