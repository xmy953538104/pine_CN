// SPDX-License-Identifier: MPL-2.0
package emu.skyline.loader

import kotlinx.serialization.json.*

/** Rebuilt safe values only: the transient Catalog/unknown native fields are never retained here. */
internal class SelectedNativeScanReport private constructor(val safe: JsonObject) {
    val schemaVersion: Int get() = safe.getValue("schema_version").jsonPrimitive.int
    val status: String get() = safe.getValue("status").jsonPrimitive.content
    val errorCode: String? get() = (safe["error_code"] as? JsonPrimitive)?.contentOrNull
    val firstIssue: String? get() = ((safe["catalog_summary"] as? JsonObject)?.get("issue_codes") as? JsonArray)
        ?.firstOrNull()?.jsonPrimitive?.content ?: (safe["error_code"] as? JsonPrimitive)?.contentOrNull

    companion object {
        const val MaxBytes = 16 * 1024
        const val MaxRows = 32
        val stages = setOf("profile_validation", "keystore_load", "fd_duplicate", "catalog_scan", "catalog_serialize")
        val codes = setOf(
        "ambiguous_content",
        "aoc_data_runtime_unsupported",
        "association_budget",
        "bktr_requires_base",
        "cnmt_digest_mismatch",
        "compression_unsupported",
        "content_attributes_unsupported",
        "content_info_layout_unknown",
        "content_size_mismatch",
        "content_type_mismatch",
        "data_patch_unsupported",
        "delta_fragment_unsupported",
        "delta_unsupported",
        "duplicate_content_id",
        "duplicate_content_reference",
        "duplicate_pfs_name",
        "encrypted_header_or_unknown_format",
        "encrypted_section_unsupported",
        "explicit_parent_ambiguous",
        "explicit_parent_invalid",
        "explicit_parent_mismatch",
        "explicit_parent_missing",
        "extended_data_unsupported",
        "hash_budget",
        "input_changed",
        "input_fd_duplicate_failed",
        "input_fd_invalid",
        "input_fd_not_readable",
        "input_not_regular",
        "input_open_failed",
        "input_read_failed",
        "install_type_unsupported",
        "integer_overflow",
        "invalid_cnmt_length",
        "invalid_cnmt_reserved",
        "invalid_integer_width",
        "invalid_nca_hash_block_size",
        "invalid_nca_hash_layer_count",
        "invalid_nca_section_range",
        "invalid_pfs_name",
        "invalid_pfs_reserved",
        "meta_cnmt_unreadable",
        "meta_has_no_cnmt",
        "meta_reference_ambiguous",
        "meta_reference_missing",
        "metadata_ambiguity",
        "metadata_hash_unsupported",
        "missing_aoc_data",
        "missing_content",
        "missing_header_key",
        "missing_key_area",
        "missing_key_store",
        "missing_program",
        "missing_title_kek",
        "missing_title_key",
        "multiple_programs",
        "native_backend_inspection_only",
        "native_content_type_unsupported",
        "native_ctr_alignment_unsupported",
        "native_invalid_header",
        "native_invalid_ivfc",
        "native_invalid_ivfc_block_order",
        "native_invalid_ivfc_reserved",
        "native_invalid_read_count",
        "native_ivfc_levels_unsupported",
        "native_key_area_unsupported",
        "native_key_generation_unsupported",
        "native_missing_input",
        "native_nca_backend_failed",
        "native_nca_parse_failed",
        "native_noncontiguous_sections_unsupported",
        "native_null_backing",
        "native_null_source",
        "native_pfs_backing_unavailable",
        "native_plaintext_unsupported",
        "native_section_count_unsupported",
        "native_section_encryption_unsupported",
        "native_section_not_validated",
        "native_size_overflow",
        "native_unreliable_backing",
        "nca_fs_header_hash_mismatch",
        "nca_hash_layers_unsupported",
        "nca_inspector_failed",
        "nca_no_sections",
        "nca_section_format_unsupported",
        "nca_size_mismatch",
        "other_native_issue",
        "out_of_range",
        "overlapping_nca_sections",
        "overlapping_pfs_entries",
        "patch_info_unsupported",
        "patch_parent_mismatch",
        "romfs_not_validated",
        "scan_budget",
        "section_data_hash_not_verified",
        "selected_content_unavailable",
        "selected_inspection_failed",
        "selected_jni_exception",
        "selected_preview_limit",
        "selected_private_path_invalid",
        "selected_private_path_required",
        "selected_profile_required",
        "selected_report_limit",
        "selected_wire_invalid",
        "selected_wire_limit",
        "short_read",
        "sparse_unsupported",
        "truncated_nca_header",
        "unclassified_meta_member",
        "unknown_content_type",
        "unsupported_cnmt_platform",
        "unsupported_cnmt_role",
        "unsupported_distribution_type",
        "unsupported_extended_header",
        "unsupported_nca_fs_version",
        "unsupported_pfs_format")
        private val roles = setOf("application", "patch", "aoc", "other")
        private val parsing = setOf("unknown", "parsed", "unsupported", "invalid", "not_inspected")
        private val digests = setOf("unknown", "unverified", "verified", "mismatch")
        private val readability = setOf("unknown", "plaintext_header", "decrypted_header", "encrypted_or_unknown", "unreadable", "not_inspected")
        private val support = setOf("unknown", "metadata_readable", "unsupported", "invalid")
        private val publicId = Regex("[0-9a-f]{16}")
        private val groupId = Regex("cnmt-sha256:[0-9a-f]{64}")
        private val contentId = Regex("[0-9a-f]{32}")
        private val decimalSize = Regex("0|[1-9][0-9]{0,19}")
        private val keyStatuses = setOf("not_started", "loading", "loaded", "failed")
        private val keyPresence = setOf("not_checked", "missing", "present", "check_failed")
        private val keyReads = setOf("not_attempted", "complete", "short", "failed", "unknown")
        private val keyLoads = setOf("not_attempted", "skipped", "loaded", "failed")
        private val keyLookups = setOf("not_observed", "not_required", "matched", "not_matched")
        private val hashObservations = setOf("not_performed", "not_reported")

        fun sanitize(source: JsonObject): SelectedNativeScanReport {
            require(source.toString().toByteArray(Charsets.UTF_8).size <= MaxBytes) { "observed_report_limit" }
            val version = source.scanInt("schema_version").also { require(it == 1L || it == 2L) }.toInt()
            val status = source.scanText("status")
            require(status in setOf("completed", "failed"))
            val phases = source.scanArray("phases")
            require(phases.size <= 5)
            val safe = buildJsonObject {
                put("schema_version", version); put("status", status)
                put("failed_stage", source["failed_stage"].scanNullableToken(stages, "unknown"))
                put("error_code", source["error_code"].scanNullableToken(codes, "other_native_issue"))
                put("elapsed_us", source.scanInt("elapsed_us"))
                put("phases", JsonArray(phases.map { raw ->
                    val phase = raw.scanObject()
                    buildJsonObject {
                        put("stage", phase.scanToken("stage", stages))
                        put("start_us", phase.scanInt("start_us")); put("duration_us", phase.scanInt("duration_us"))
                        put("status", phase.scanToken("status", setOf("begun", "completed", "failed")))
                    }
                }))
                val summary = source.getValue("catalog_summary")
                put("catalog_summary", if (summary == JsonNull) JsonNull else sanitizeSummary(summary.scanObject(), version))
                if (version == 2) put("key_state", sanitizeKeyState(source.getValue("key_state").scanObject()))
            }
            require(safe.toString().toByteArray(Charsets.UTF_8).size <= MaxBytes)
            return SelectedNativeScanReport(safe)
        }

        private fun sanitizeKeyState(source: JsonObject): JsonObject = buildJsonObject {
            val status = source.scanExactToken("status", keyStatuses)
            put("status", status)
            var incomplete = status != "loaded"
            listOf("title_keys", "prod_keys").forEach { file ->
                val state = source.getValue(file).scanObject()
                val presence = state.scanExactToken("presence", keyPresence)
                val read = state.scanExactToken("read", keyReads)
                val load = state.scanExactToken("load", keyLoads)
                if (status == "not_started") require(presence == "not_checked" && read == "not_attempted" && load == "not_attempted")
                incomplete = incomplete || presence == "check_failed" || read in setOf("short", "unknown", "failed") || load == "failed"
                put(file, buildJsonObject {
                    put("presence", presence); put("read", read); put("load", load)
                })
            }
            listOf("loaded_unique_title_key_count" to Int.MAX_VALUE.toLong(), "recognized_prod_slot_count" to 81L).forEach { (field, max) ->
                val raw = source.getValue(field)
                put(field, if (raw == JsonNull) JsonNull else JsonPrimitive(source.scanInt(field).also { require(it <= max) }))
            }
            val partial = source.scanBool("counts_partial")
            require(!incomplete || partial)
            if (!partial) require(source.getValue("loaded_unique_title_key_count") != JsonNull && source.getValue("recognized_prod_slot_count") != JsonNull)
            if (status == "not_started") {
                require(source.getValue("loaded_unique_title_key_count") == JsonNull && source.getValue("recognized_prod_slot_count") == JsonNull && partial)
            }
            put("counts_partial", partial)
        }

        private fun sanitizeSummary(source: JsonObject, version: Int): JsonObject {
            val groups = source.scanArray("groups")
            val members = source.scanArray("members")
            val selectedGroups = groups.take(MaxRows)
            val selectedMembers = members.take(MaxRows - selectedGroups.size)
            return buildJsonObject {
                if (version == 2) {
                    put("container_scope", source.scanExactToken("container_scope", setOf("single_container")))
                    require(!source.scanBool("includes_external_associations"))
                    put("includes_external_associations", false)
                    put("container_size_bytes", source.scanSize("container_size_bytes"))
                    put("full_content_hash", source.scanExactToken("full_content_hash", hashObservations))
                }
                put("bounded_scan_complete", source.scanBool("bounded_scan_complete"))
                put("all_metadata_enumerated", source.scanBool("all_metadata_enumerated"))
                listOf("nca_count", "group_count", "other_entry_count", "meta_nca_count", "unknown_nca_count", "unreadable_nca_count")
                    .forEach { put(it, source.scanCount(it)) }
                val counts = source.getValue("role_counts").scanObject()
                put("role_counts", buildJsonObject { roles.forEach { put(it, counts.scanCount(it)) } })
                put("issue_codes", issueCodes(source))
                put("groups", JsonArray(selectedGroups.map { raw ->
                    val group = raw.scanObject()
                    val parsed = group.scanToken("parsing", parsing)
                    buildJsonObject {
                        put("ordinal", group.scanCount("ordinal")); put("role", group.scanToken("role", roles, "other"))
                        put("parsing", parsed); put("digest_observation", group.scanToken("digest_observation", digests))
                        put("issue_codes", issueCodes(group))
                        if (parsed == "parsed") {
                            if (version == 2 && "version" in group) put("version", group.scanInt("version").also { require(it <= 0xffffffffL) })
                            listOf("title_id", "application_id", "cnmt_group_id").forEach { field ->
                                val value = (group[field] as? JsonPrimitive)?.takeIf { it.isString }?.content
                                if (value != null && (if (field == "cnmt_group_id") groupId else publicId).matches(value)) put(field, value)
                            }
                        }
                    }
                }))
                put("members", JsonArray(selectedMembers.map { raw ->
                    val member = raw.scanObject()
                    buildJsonObject {
                        put("ordinal", member.scanCount("ordinal"))
                        val type = member.getValue("nca_type")
                        put("nca_type", if (type == JsonNull) JsonNull else JsonPrimitive(member.scanInt("nca_type").also { require(it <= 255) }))
                        put("readability", member.scanToken("readability", readability)); put("support", member.scanToken("support", support))
                        put("issue_codes", issueCodes(member))
                        if (version == 2) {
                            put("size_bytes", member.scanSize("size_bytes"))
                            put("size_source", member.scanExactToken("size_source", setOf("container_member_extent")))
                            put("container_scope", member.scanExactToken("container_scope", setOf("single_container")))
                            put("external_lookup", member.scanExactToken("external_lookup", setOf("not_performed")))
                            put("full_hash_verification", member.scanExactToken("full_hash_verification", hashObservations))
                            val lookup = member.scanExactToken("title_key_lookup", keyLookups)
                            val evidence = member.scanExactToken("title_key_lookup_evidence", setOf("read_only_loaded_map_probe", "not_performed"))
                            require(evidence == if (lookup == "not_observed") "not_performed" else "read_only_loaded_map_probe")
                            put("title_key_lookup", lookup); put("title_key_lookup_evidence", evidence)
                            if (type != JsonNull && member.scanInt("nca_type") in setOf(0L, 5L)) {
                                val hint = (member["content_id_hint"] as? JsonPrimitive)?.takeIf { it.isString }?.content
                                val provenance = (member["content_id_source"] as? JsonPrimitive)?.takeIf { it.isString }?.content
                                if (hint != null && contentId.matches(hint) && provenance == "entry_name_hint") {
                                    put("content_id_hint", hint); put("content_id_source", "entry_name_hint")
                                }
                            }
                        }
                    }
                }))
                put("groups_omitted", minOf(Int.MAX_VALUE.toLong(), source.scanCount("groups_omitted") + groups.size - selectedGroups.size))
                put("members_omitted", minOf(Int.MAX_VALUE.toLong(), source.scanCount("members_omitted") + members.size - selectedMembers.size))
            }.also { safe ->
                if (version == 2) require(listOf("groups", "members").sumOf { field ->
                    safe.getValue(field).jsonArray.sumOf { row -> row.toString().toByteArray(Charsets.UTF_8).size }
                } <= 12 * 1024)
            }
        }

        private fun JsonObject.scanSize(field: String): String = scanText(field).also {
            require(decimalSize.matches(it) && it.toULongOrNull() != null)
        }

        private fun issueCodes(source: JsonObject): JsonArray = JsonArray(source.scanArray("issue_codes").take(8)
            .map { raw -> val code = (raw as? JsonPrimitive)?.takeIf { it.isString }?.content
                JsonPrimitive(if (code in codes) code!! else "other_native_issue") }.distinct())
    }
}

internal class SelectedScanFailure(val code: String, val report: SelectedNativeScanReport? = null) : IllegalArgumentException(code)
internal data class SelectedObservedScan(val catalog: SelectedContentCatalog?, val report: SelectedNativeScanReport) {
    companion object {
        const val MaxEnvelopeChars = SelectedContentCatalog.MaxJsonChars + SelectedNativeScanReport.MaxBytes + 1024
        fun parse(text: String, profile: SelectedContentProfile): SelectedObservedScan {
            try { checkJsonEnvelope(text, MaxEnvelopeChars, 16) }
            catch (error: Exception) { throw SelectedScanFailure(if (text.length > MaxEnvelopeChars) "observed_json_limit" else "observed_json_invalid") }
            val envelope = try { Json.parseToJsonElement(text).scanObject() }
                catch (error: Exception) { throw SelectedScanFailure("observed_json_invalid") }
            val report = try {
                require(envelope.scanInt("selected_scan_schema") == 1L)
                SelectedNativeScanReport.sanitize(envelope.getValue("scan_report").scanObject()).also {
                    require(it.status == envelope.scanText("status"))
                }
            } catch (error: Exception) { throw SelectedScanFailure("observed_report_invalid") }
            if (report.status == "failed") {
                if (envelope["catalog"] != JsonNull) throw SelectedScanFailure("observed_report_invalid", report)
                return SelectedObservedScan(null, report)
            }
            val catalog = try {
                SelectedContentCatalog.parse(envelope.getValue("catalog").scanObject().toString(), profile)
            } catch (error: Exception) { throw SelectedScanFailure("catalog_json_invalid", report) }
            return SelectedObservedScan(catalog, report)
        }
    }
}

private fun JsonElement.scanObject() = this as? JsonObject ?: throw IllegalArgumentException("observed_object")
private fun JsonObject.scanText(key: String): String = (getValue(key) as? JsonPrimitive)?.also { require(it.isString) }?.content
    ?: throw IllegalArgumentException("observed_string")
private fun JsonObject.scanToken(key: String, allowed: Set<String>, unknown: String = "unknown") = scanText(key).let { if (it in allowed) it else unknown }
private fun JsonObject.scanExactToken(key: String, allowed: Set<String>) = scanText(key).also { require(it in allowed) }
private fun JsonElement?.scanNullableToken(allowed: Set<String>, unknown: String): JsonElement =
    if (this == JsonNull) JsonNull else JsonPrimitive((this as? JsonPrimitive)?.takeIf { it.isString }?.content.let { if (it in allowed) it!! else unknown })
private fun JsonObject.scanInt(key: String): Long = (getValue(key) as? JsonPrimitive)?.also { require(!it.isString) }
    ?.longOrNull?.also { require(it >= 0) } ?: throw IllegalArgumentException("observed_integer")
private fun JsonObject.scanCount(key: String): Long = scanInt(key).also { require(it <= Int.MAX_VALUE) }
private fun JsonObject.scanBool(key: String) = (getValue(key) as? JsonPrimitive)?.also { require(!it.isString) }
    ?.booleanOrNull ?: throw IllegalArgumentException("observed_boolean")
private fun JsonObject.scanArray(key: String) = getValue(key) as? JsonArray ?: throw IllegalArgumentException("observed_array")
