// SPDX-License-Identifier: MPL-2.0
package emu.skyline.loader

import kotlinx.serialization.json.*
import org.junit.Assert.*
import org.junit.Test
import java.io.File
import java.security.MessageDigest

/** Constructed v2 boundary cases; exact production reports are a separate resource test. */
internal object SelectedKeyStateFixture {
    fun contract() = Json.parseToJsonElement(File("../tests/selected-scan/report-contract-v2.json").readText()).jsonObject
    fun report() = contract().getValue("example_envelope").jsonObject.getValue("scan_report").jsonObject
    fun loadedReport(lookup: String = "not_matched"): JsonObject {
        val original = report()
        val summary = original.getValue("catalog_summary").jsonObject
        val member = summary.getValue("members").jsonArray.single().jsonObject
        return JsonObject(original + mapOf("key_state" to buildJsonObject {
            put("status", "loaded"); put("counts_partial", false)
            listOf("title_keys", "prod_keys").forEach { field -> put(field, buildJsonObject {
                put("presence", "present"); put("read", "complete"); put("load", "loaded")
            }) }
            put("loaded_unique_title_key_count", 1); put("recognized_prod_slot_count", 2)
        }, "catalog_summary" to JsonObject(summary + mapOf("groups" to JsonArray(listOf(buildJsonObject {
            put("ordinal", 0); put("role", "application"); put("parsing", "parsed"); put("digest_observation", "mismatch")
            put("version", 4294967295L); put("title_id", "0100000000001000"); put("issue_codes", JsonArray(emptyList()))
        })), "members" to JsonArray(listOf(JsonObject(member + mapOf("nca_type" to JsonPrimitive(0),
            "title_key_lookup" to JsonPrimitive(lookup), "title_key_lookup_evidence" to JsonPrimitive(if (lookup == "not_observed") "not_performed" else "read_only_loaded_map_probe"),
            "content_id_hint" to JsonPrimitive("0123456789abcdef0123456789abcdef"), "content_id_source" to JsonPrimitive("entry_name_hint")))))))))
    }
    fun summary(report: JsonObject = loadedReport()) = report.getValue("catalog_summary").jsonObject
    fun replaceSummary(report: JsonObject, summary: JsonObject) = JsonObject(report + ("catalog_summary" to summary))
    fun replaceMember(report: JsonObject, transform: (JsonObject) -> JsonObject): JsonObject {
        val summary = summary(report)
        return replaceSummary(report, JsonObject(summary + ("members" to JsonArray(listOf(transform(summary.getValue("members").jsonArray.single().jsonObject))))))
    }
}

class SelectedKeyStateTest {
    private fun rejected(source: JsonObject) { assertTrue(runCatching { SelectedNativeScanReport.sanitize(source) }.isFailure) }

    @Test fun exactRealNativeJniV2ReportsPreserveFileStatesCountsAndHeaderProbeBoundaries() {
        fun bytes(name: String) = requireNotNull(javaClass.getResourceAsStream("/selected-scan/native-reports-v2/$name")).use { it.readBytes() }
        val hashes = Json.parseToJsonElement(bytes("sha256.json").toString(Charsets.UTF_8)).jsonObject
        val reports = hashes.mapValues { (name, digest) ->
            val raw = bytes(name)
            assertEquals(digest.jsonPrimitive.content, MessageDigest.getInstance("SHA-256").digest(raw).joinToString("") { "%02x".format(it) })
            val original = Json.parseToJsonElement(raw.toString(Charsets.UTF_8)).jsonObject
            SelectedNativeScanReport.sanitize(original).safe.also { assertEquals(original, it) }
        }
        val valid = reports.getValue("valid-modern.json")
        assertEquals("missing", valid.getValue("key_state").jsonObject.getValue("title_keys").jsonObject.getValue("presence").jsonPrimitive.content)
        assertEquals(2, valid.getValue("key_state").jsonObject.getValue("recognized_prod_slot_count").jsonPrimitive.int)
        assertEquals(1, SelectedKeyStateFixture.summary(valid).getValue("groups").jsonArray.single().jsonObject.getValue("version").jsonPrimitive.int)
        assertEquals("not_required", SelectedKeyStateFixture.summary(valid).getValue("members").jsonArray.single().jsonObject.getValue("title_key_lookup").jsonPrimitive.content)
        val unknown = SelectedKeyStateFixture.summary(reports.getValue("missing-header-key.json")).getValue("members").jsonArray.single().jsonObject
        assertEquals("not_observed", unknown.getValue("title_key_lookup").jsonPrimitive.content)
        assertEquals("not_performed", unknown.getValue("title_key_lookup_evidence").jsonPrimitive.content)
        val failed = reports.getValue("unknown-native-error.json")
        assertEquals("failed", failed.getValue("key_state").jsonObject.getValue("prod_keys").jsonObject.getValue("load").jsonPrimitive.content)
        assertTrue(failed.getValue("key_state").jsonObject.getValue("counts_partial").jsonPrimitive.boolean)
        assertEquals("other_native_issue", failed.getValue("error_code").jsonPrimitive.content)
        assertFalse(reports.values.any { it.containsKey("catalog") || it.toString().contains("PRIVATE_CANARY") })
    }

    @Test fun exactV2ContractAndV1HistoricalShapesAreBothRebuiltWithoutUpgradingHistory() {
        val contract = SelectedKeyStateFixture.contract()
        assertEquals(SelectedNativeScanReport.codes, contract.getValue("code_allowlist").jsonArray.map { it.jsonPrimitive.content }.toSet())
        val example = SelectedKeyStateFixture.report()
        assertEquals(example, SelectedNativeScanReport.sanitize(example).safe)
        assertEquals(2, SelectedNativeScanReport.sanitize(example).schemaVersion)
        val historical = SelectedScanTestFixture.report()
        assertEquals(historical, SelectedNativeScanReport.sanitize(historical).safe)
        assertFalse("key_state" in SelectedNativeScanReport.sanitize(historical).safe)
        rejected(JsonObject(example + ("schema_version" to JsonPrimitive(3))))
        rejected(JsonObject(example - "key_state"))
    }

    @Test fun presentFilesAndStrictCountsAreIndependentOfTargetMatchAndDecryptSupport() {
        val source = SelectedKeyStateFixture.loadedReport()
        val safe = SelectedNativeScanReport.sanitize(source).safe
        assertEquals(source, safe)
        assertEquals("present", safe.getValue("key_state").jsonObject.getValue("title_keys").jsonObject.getValue("presence").jsonPrimitive.content)
        val member = SelectedKeyStateFixture.summary(safe).getValue("members").jsonArray.single().jsonObject
        assertEquals("not_matched", member.getValue("title_key_lookup").jsonPrimitive.content)
        assertEquals("read_only_loaded_map_probe", member.getValue("title_key_lookup_evidence").jsonPrimitive.content)
        assertEquals("unsupported", member.getValue("support").jsonPrimitive.content)
        val matched = SelectedNativeScanReport.sanitize(SelectedKeyStateFixture.loadedReport("matched")).safe
        assertEquals("unsupported", SelectedKeyStateFixture.summary(matched).getValue("members").jsonArray.single().jsonObject.getValue("support").jsonPrimitive.content)
        // An internal caller can supply an already constructed store: file-load observation remains not_started.
        val externalStore = JsonObject(SelectedKeyStateFixture.loadedReport("matched") +
            ("key_state" to SelectedKeyStateFixture.report().getValue("key_state")))
        assertEquals(externalStore, SelectedNativeScanReport.sanitize(externalStore).safe)
        val before = SelectedObservedScan.parse(SelectedScanTestFixture.envelope(), SelectedContentProfile.Modern40).catalog!!
        val after = SelectedObservedScan.parse(SelectedScanTestFixture.envelope(report = source), SelectedContentProfile.Modern40).catalog!!
        assertEquals(primaryState(before).primaryRows.map { it.predicate }, primaryState(after).primaryRows.map { it.predicate })
        assertArrayEquals(before.select(SelectedPreviewFixture.primary(before), null, emptyList()).toNativeWire().fields,
            after.select(SelectedPreviewFixture.primary(after), null, emptyList()).toNativeWire().fields)
    }

    @Test fun uint32VersionsUInt64CanonicalSizesAndFixedCountsRejectOutOfRangeValues() {
        val source = SelectedKeyStateFixture.loadedReport()
        val summary = SelectedKeyStateFixture.summary(source)
        for (size in listOf("0", "18446744073709551615")) {
            val changed = SelectedKeyStateFixture.replaceSummary(source, JsonObject(summary + ("container_size_bytes" to JsonPrimitive(size))))
            assertEquals(size, SelectedKeyStateFixture.summary(SelectedNativeScanReport.sanitize(changed).safe).getValue("container_size_bytes").jsonPrimitive.content)
        }
        for (size in listOf("18446744073709551616", "-1", "+1", "01", " 1", "1.0", "0x10", "")) {
            rejected(SelectedKeyStateFixture.replaceSummary(source, JsonObject(summary + ("container_size_bytes" to JsonPrimitive(size)))))
            rejected(SelectedKeyStateFixture.replaceMember(source) { JsonObject(it + ("size_bytes" to JsonPrimitive(size))) })
        }
        for (version in listOf(-1L, 4294967296L)) rejected(SelectedKeyStateFixture.replaceSummary(source,
            JsonObject(summary + ("groups" to JsonArray(listOf(JsonObject(summary.getValue("groups").jsonArray.single().jsonObject + ("version" to JsonPrimitive(version)))))))))
        val state = source.getValue("key_state").jsonObject
        for ((field, value) in listOf("loaded_unique_title_key_count" to 2147483648L, "recognized_prod_slot_count" to 82L, "recognized_prod_slot_count" to -1L))
            rejected(JsonObject(source + ("key_state" to JsonObject(state + (field to JsonPrimitive(value))))))
        rejected(JsonObject(SelectedKeyStateFixture.report() + ("key_state" to JsonObject(SelectedKeyStateFixture.report().getValue("key_state").jsonObject + ("counts_partial" to JsonPrimitive(false))))))
    }

    @Test fun safePublicHintsArePairedTypedAndNeverBecomeCnmtHashOrPrivateEvidence() {
        val source = SelectedKeyStateFixture.loadedReport()
        fun member(changed: JsonObject) = SelectedKeyStateFixture.summary(SelectedNativeScanReport.sanitize(changed).safe).getValue("members").jsonArray.single().jsonObject
        for (type in listOf(1, 2, 3, 4, 255)) assertFalse("content_id_hint" in member(SelectedKeyStateFixture.replaceMember(source) { JsonObject(it + ("nca_type" to JsonPrimitive(type))) }))
        assertTrue("content_id_hint" in member(SelectedKeyStateFixture.replaceMember(source) { JsonObject(it + ("nca_type" to JsonPrimitive(5))) }))
        for (hint in listOf("A".repeat(32), "a".repeat(31), "file:///PRIVATE_CANARY", "a".repeat(64)))
            assertFalse("content_id_hint" in member(SelectedKeyStateFixture.replaceMember(source) { JsonObject(it + ("content_id_hint" to JsonPrimitive(hint))) }))
        assertFalse("content_id_hint" in member(SelectedKeyStateFixture.replaceMember(source) { JsonObject(it - "content_id_source") }))
        val summary = SelectedKeyStateFixture.summary(source)
        val unparsed = JsonObject(summary.getValue("groups").jsonArray.single().jsonObject + ("parsing" to JsonPrimitive("invalid")))
        val safe = SelectedNativeScanReport.sanitize(SelectedKeyStateFixture.replaceSummary(source, JsonObject(summary + ("groups" to JsonArray(listOf(unparsed)))))).safe
        assertFalse("version" in SelectedKeyStateFixture.summary(safe).getValue("groups").jsonArray.single().jsonObject)
        rejected(SelectedKeyStateFixture.replaceMember(source) { JsonObject(it + ("title_key_lookup_evidence" to JsonPrimitive("not_performed"))) })
        rejected(SelectedKeyStateFixture.replaceSummary(source, JsonObject(summary + ("includes_external_associations" to JsonPrimitive(true)))))
    }

    @Test fun unknownPrivateFieldsAreDiscardedAndNewStateEnumsCannotInventSuccess() {
        val source = SelectedKeyStateFixture.loadedReport()
        val canary = "PRIVATE_CANARY_DO_NOT_EXPORT"
        val state = source.getValue("key_state").jsonObject
        val polluted = JsonObject(source + mapOf("key" to JsonPrimitive(canary), "catalog" to buildJsonObject { put("private", canary) },
            "key_state" to JsonObject(state + mapOf("path" to JsonPrimitive("/data/$canary"), "rights_id" to JsonPrimitive(canary),
                "title_keys" to JsonObject(state.getValue("title_keys").jsonObject + ("line" to JsonPrimitive(canary)))))))
        val safe = SelectedNativeScanReport.sanitize(polluted).safe
        assertEquals(source, safe); assertFalse(safe.toString().contains(canary))
        rejected(JsonObject(source + ("key_state" to JsonObject(state + ("status" to JsonPrimitive(canary))))))
        rejected(SelectedKeyStateFixture.replaceMember(source) { JsonObject(it + ("title_key_lookup" to JsonPrimitive(canary))) })
        rejected(SelectedKeyStateFixture.replaceMember(source) { JsonObject(it + ("full_hash_verification" to JsonPrimitive("verified"))) })
    }

    @Test fun v2RetainsReportRowAndRecordLimits() {
        val source = SelectedKeyStateFixture.loadedReport(); val summary = SelectedKeyStateFixture.summary(source)
        rejected(JsonObject(source + ("private" to JsonPrimitive("x".repeat(SelectedNativeScanReport.MaxBytes)))))
        val group = summary.getValue("groups").jsonArray.single()
        val many = SelectedKeyStateFixture.replaceSummary(source, JsonObject(summary + mapOf("groups" to JsonArray(List(33) { group }), "members" to JsonArray(emptyList()))))
        val safe = SelectedKeyStateFixture.summary(SelectedNativeScanReport.sanitize(many).safe)
        assertEquals(32, safe.getValue("groups").jsonArray.size); assertEquals(1, safe.getValue("groups_omitted").jsonPrimitive.int)
        val record = SelectedScanTestFixture.record(fields = SelectedScanTestFixture.fields(source))
        assertTrue(record.toString().toByteArray().size + 1 <= emu.skyline.diagnostics.SelectedScanRecord.MaxRecordBytes)
        assertEquals(2, record.getValue("fields").jsonObject.getValue("native_report").jsonObject.getValue("schema_version").jsonPrimitive.int)
        val member = summary.getValue("members").jsonArray.single().jsonObject
        val rich = JsonObject(member + ("issue_codes" to JsonArray(SelectedNativeScanReport.codes.sortedByDescending { it.length }.take(8).map(::JsonPrimitive))))
        val tooManyBytes = SelectedKeyStateFixture.replaceSummary(source, JsonObject(summary +
            mapOf("groups" to JsonArray(emptyList()), "members" to JsonArray(List(20) { rich }))))
        assertTrue(tooManyBytes.toString().toByteArray().size <= SelectedNativeScanReport.MaxBytes)
        assertTrue(List(20) { rich }.sumOf { it.toString().toByteArray().size } > 12 * 1024)
        rejected(tooManyBytes)
    }

    @Test fun readAndLoadFailuresRetainPartialCountsWithoutRewritingTheirMeaning() {
        val source = SelectedKeyStateFixture.loadedReport()
        val state = source.getValue("key_state").jsonObject
        for (read in listOf("complete", "short", "failed", "unknown", "not_attempted")) {
            val partial = JsonObject(state + mapOf("status" to JsonPrimitive("failed"), "counts_partial" to JsonPrimitive(true),
                "title_keys" to buildJsonObject { put("presence", "present"); put("read", read); put("load", "failed") }))
            val safe = SelectedNativeScanReport.sanitize(JsonObject(source + ("key_state" to partial))).safe
            assertEquals(partial, safe.getValue("key_state"))
        }
        val skipped = JsonObject(state + ("title_keys" to buildJsonObject { put("presence", "present"); put("read", "not_attempted"); put("load", "skipped") }))
        assertEquals(skipped, SelectedNativeScanReport.sanitize(JsonObject(source + ("key_state" to skipped))).safe.getValue("key_state"))
        for (status in listOf("failed", "loading")) rejected(JsonObject(source + ("key_state" to JsonObject(state + ("status" to JsonPrimitive(status))))))
        for (field in listOf("loaded_unique_title_key_count", "recognized_prod_slot_count")) {
            rejected(JsonObject(source + ("key_state" to JsonObject(state + (field to JsonNull)))))
            val unknown = JsonObject(state + mapOf(field to JsonNull, "counts_partial" to JsonPrimitive(true)))
            assertEquals(unknown, SelectedNativeScanReport.sanitize(JsonObject(source + ("key_state" to unknown))).safe.getValue("key_state"))
        }
        val unstarted = SelectedKeyStateFixture.report().getValue("key_state").jsonObject
        rejected(JsonObject(source + ("key_state" to JsonObject(unstarted + ("title_keys" to state.getValue("title_keys"))))))
        val file = state.getValue("title_keys").jsonObject
        for ((field, bad) in listOf("read" to "short", "read" to "unknown", "read" to "failed", "presence" to "check_failed", "load" to "failed")) {
            val incomplete = JsonObject(state + ("title_keys" to JsonObject(file + (field to JsonPrimitive(bad)))))
            rejected(JsonObject(source + ("key_state" to incomplete)))
            assertEquals(true, SelectedNativeScanReport.sanitize(JsonObject(source + ("key_state" to JsonObject(incomplete +
                ("counts_partial" to JsonPrimitive(true)))))).safe.getValue("key_state").jsonObject.getValue("counts_partial").jsonPrimitive.boolean)
        }
        val unreported = SelectedKeyStateFixture.replaceSummary(source, JsonObject(SelectedKeyStateFixture.summary(source) + ("full_content_hash" to JsonPrimitive("not_reported"))))
        assertEquals("not_reported", SelectedKeyStateFixture.summary(SelectedNativeScanReport.sanitize(unreported).safe).getValue("full_content_hash").jsonPrimitive.content)
    }
}
