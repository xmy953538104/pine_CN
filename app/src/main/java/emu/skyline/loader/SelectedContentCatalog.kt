// SPDX-License-Identifier: MPL-2.0
package emu.skyline.loader

import kotlinx.serialization.json.*

enum class SelectedRejection {
    IncompleteCatalog, UnknownRole, InvalidMetadata, Ambiguous, MissingContent, UnsupportedContent,
    BktrUnsupported, ProgramIdentityUnknown, ControlRequired, PublicDataRequired, ParentMismatch, UpdateIdentityMismatch
}

data class SelectedCatalogSection(val fsType: Int, val hashType: Int, val requiresBase: Boolean,
                                  val sparse: Boolean, val compressed: Boolean, val encryptionType: Int)
data class SelectedCatalogNca(val id: String, val size: ULong, val type: Int?, val programId: String?,
                              val support: String, val sections: List<SelectedCatalogSection>, val issues: Set<String>)
data class SelectedCatalogReference(val contentId: String, val type: Int, val idOffset: Int,
                                    val size: ULong?, val actualSize: ULong?, val actualType: Int?,
                                    val member: String?, val candidates: List<String>, val consistency: String,
                                    val attributes: Int?, val hashVerification: String)
data class SelectedCatalogProgram(val choice: SelectedProgramChoice?, val contentId: String,
                                  val idOffset: Int, val requiresBase: Boolean)
data class SelectedCatalogGroup(val id: String, val titleId: String?, val version: Long?, val type: Int?,
    val applicationId: String?, val patchId: String?, val dataPatchId: String?, val parsing: String,
    val digest: String, val installType: Int?, val platform: Int?, val attributes: Int?,
    val storageId: Int?, val committed: Int?, val extendedData: String,
    val references: List<SelectedCatalogReference>, val programs: List<SelectedCatalogProgram>,
    val metaReferencesResolved: Boolean, val issues: Set<String>) {
    val applicationTarget: String? get() = if (type == 128) titleId else applicationId
}

data class SelectedProgramCandidate(val group: SelectedCatalogGroup, val program: SelectedCatalogProgram,
                                     val rejection: SelectedRejection?, val predicate: String? = null)
data class SelectedGroupCandidate(val group: SelectedCatalogGroup, val rejection: SelectedRejection?, val predicate: String? = null,
    val ordinal: Int = -1)

/** Metadata observations are candidates, never successful launch/signature/hash verification. */
class SelectedContentCatalog private constructor(val profile: SelectedContentProfile,
    val groups: List<SelectedCatalogGroup>, val ncas: Map<String, SelectedCatalogNca>,
    val complete: Boolean, val nativeInspector: Boolean) {

    private data class Decision(val rejection: SelectedRejection, val predicate: String)

    // Each predicate is recorded at its original short-circuit decision point.
    // The order and returned eligibility remain the selection policy.
    private fun groupDecision(group: SelectedCatalogGroup): Decision? {
        fun reject(reason: SelectedRejection, predicate: String) = Decision(reason, predicate)
        if (!complete) return reject(SelectedRejection.IncompleteCatalog, "catalog_incomplete")
        if (!nativeInspector) return reject(SelectedRejection.IncompleteCatalog, "native_inspector_required")
        if (group.type !in setOf(128, 129, 130)) return reject(SelectedRejection.UnknownRole, "unsupported_group_role")
        val metadata = when {
            group.parsing != "parsed" -> "group_not_parsed"
            group.titleId == null -> "title_id_missing"
            group.titleId.all { it == '0' } -> "title_id_zero"
            group.version == null -> "version_missing"
            group.digest != "verified" -> "cnmt_digest_not_verified"
            group.installType != 0 -> "install_type_not_full"
            group.platform != 0 -> "platform_unsupported"
            group.attributes != 0 -> "group_attributes_unsupported"
            group.storageId == null || group.storageId !in 0..6 -> "storage_id_unknown"
            group.committed == null || group.committed !in 0..1 -> "committed_unknown"
            group.extendedData != "none" -> "extended_data_unsupported"
            group.type != 128 && (group.applicationId == null || group.applicationId.all { it == '0' }) -> "explicit_parent_missing"
            else -> null
        }
        if (metadata != null) return reject(SelectedRejection.InvalidMetadata, metadata)
        if (group.issues.any { it in setOf("metadata_ambiguity", "duplicate_content_reference") })
            return reject(SelectedRejection.Ambiguous, "metadata_or_reference_ambiguous")
        if (group.programs.map { it.idOffset }.distinct().size != group.programs.size)
            return reject(SelectedRejection.Ambiguous, "duplicate_program_offset")
        if (!group.metaReferencesResolved) return reject(SelectedRejection.MissingContent, "meta_reference_unresolved")
        if (group.references.isEmpty()) return reject(SelectedRejection.MissingContent, "content_table_empty")
        for (reference in group.references) {
            val missing = when {
                reference.candidates.size != 1 -> "reference_candidate_count"
                reference.member != reference.candidates.singleOrNull() -> "reference_member_mismatch"
                reference.consistency != "consistent" -> "reference_inconsistent"
                reference.size == null -> "reference_size_missing"
                reference.size != reference.actualSize -> "reference_size_mismatch"
                else -> null
            }
            if (missing != null) return reject(SelectedRejection.MissingContent, missing)
            val nca = ncas[reference.member] ?: return reject(SelectedRejection.MissingContent, "referenced_member_missing")
            if (nca.issues.contains("duplicate_content_id")) return reject(SelectedRejection.Ambiguous, "duplicate_content_id")
            if (nca.sections.any { it.requiresBase }) return reject(SelectedRejection.BktrUnsupported, "section_requires_base")
            val expectedType = listOf(1, 0, 4, 2, 3, 3).getOrNull(reference.type)
                ?: return reject(SelectedRejection.UnsupportedContent, "content_type_unsupported")
            val publicData = reference.type == 2 && reference.actualType == 5
            val unsupported = when {
                !publicData && reference.actualType != expectedType -> "reference_nca_type_mismatch"
                reference.actualType != nca.type -> "member_nca_type_mismatch"
                nca.size != reference.size -> "member_size_mismatch"
                nca.support != "metadata_readable" -> "member_not_metadata_readable"
                reference.attributes?.let { it != 0 } == true -> "content_attributes_unsupported"
                profile == SelectedContentProfile.Modern40 && reference.attributes == null -> "content_attributes_missing"
                reference.contentId.all { it == '0' } -> "content_id_zero"
                reference.hashVerification !in setOf("unverified", "verified") -> "content_hash_state_unsupported"
                else -> null
            }
            if (unsupported != null) return reject(SelectedRejection.UnsupportedContent, unsupported)
            for (section in nca.sections) {
                val predicate = when {
                    section.sparse -> "sparse_section"
                    section.compressed -> "compressed_section"
                    section.encryptionType !in setOf(1, 3) -> "section_encryption_unsupported"
                    else -> null
                }
                if (predicate != null) return reject(SelectedRejection.UnsupportedContent, predicate)
            }
        }
        if (group.type == 130) {
            val data = group.references.singleOrNull()
            val predicate = when {
                data?.type != 2 -> "aoc_data_reference_not_unique"
                data.actualType != 5 -> "aoc_public_data_required"
                ncas[data.member]?.programId != group.titleId -> "aoc_data_id_mismatch"
                group.applicationId == null -> "aoc_parent_missing"
                group.dataPatchId?.any { it != '0' } == true -> "aoc_data_patch_unsupported"
                else -> null
            }
            if (predicate != null) return reject(SelectedRejection.PublicDataRequired, predicate)
        } else if (group.references.count { it.type == 3 } != 1) return reject(SelectedRejection.ControlRequired, "control_not_unique")
        return null
    }

    fun primaryGroups(): List<SelectedGroupCandidate> = groups.mapIndexedNotNull { ordinal, group ->
        if (group.type != 128 && group.type != 129) return@mapIndexedNotNull null
        val decision = groupDecision(group) ?: if (group.programs.isEmpty()) Decision(SelectedRejection.ProgramIdentityUnknown, "program_table_empty") else null
        SelectedGroupCandidate(group, decision?.rejection, decision?.predicate, ordinal)
    }

    fun programs(groupId: String): List<SelectedProgramCandidate> {
        val group = requireNotNull(groups.singleOrNull { it.id == groupId }) { "selected_group_not_available" }
        return group.programs.map { program ->
            val reference = group.references.singleOrNull { it.type == 1 && it.contentId == program.contentId && it.idOffset == program.idOffset }
            val nca = reference?.member?.let(ncas::get)
            val decision = groupDecision(group) ?: when {
                program.requiresBase -> Decision(SelectedRejection.BktrUnsupported, "program_requires_base")
                program.choice == null -> Decision(SelectedRejection.ProgramIdentityUnknown, "program_identity_unknown")
                nca == null -> Decision(SelectedRejection.ProgramIdentityUnknown, "program_member_missing")
                nca.programId != program.choice.programId -> Decision(SelectedRejection.ProgramIdentityUnknown, "program_id_mismatch")
                nca.sections.none { it.fsType == 1 && it.hashType == 2 } -> Decision(SelectedRejection.UnsupportedContent, "program_exefs_section_missing")
                nca.sections.count { it.fsType == 0 && it.hashType == 3 } != 1 -> Decision(SelectedRejection.UnsupportedContent, "program_romfs_not_unique")
                else -> null
            }
            SelectedProgramCandidate(group, program, decision?.rejection, decision?.predicate)
        }
    }

    private fun requirePrimary(choice: SelectedProgramChoice): SelectedProgramCandidate = programs(choice.groupId)
        .singleOrNull { it.program.choice == choice.checked() && it.rejection == null }
        ?: throw IllegalArgumentException("selected_primary_not_available")

    fun updates(primary: SelectedProgramChoice): List<SelectedProgramCandidate> {
        val base = requirePrimary(primary)
        if (base.group.type != 128) return emptyList()
        return groups.filter { it.type == 129 }.flatMap { programs(it.id) }.map { candidate ->
            val decision = when {
                candidate.rejection != null -> null
                candidate.group.applicationId != base.group.titleId -> Decision(SelectedRejection.ParentMismatch, "patch_parent_mismatch")
                base.group.patchId?.let { id -> id.any { it != '0' } && id != candidate.group.titleId } == true -> Decision(SelectedRejection.ParentMismatch, "application_patch_id_mismatch")
                candidate.program.choice?.index != primary.index -> Decision(SelectedRejection.UpdateIdentityMismatch, "update_program_index_mismatch")
                candidate.program.choice?.programId != primary.programId -> Decision(SelectedRejection.UpdateIdentityMismatch, "update_program_id_mismatch")
                else -> null
            }
            if (decision == null) candidate else candidate.copy(rejection = decision.rejection, predicate = decision.predicate)
        }
    }

    fun addOns(primary: SelectedProgramChoice): List<SelectedGroupCandidate> {
        val parent = requirePrimary(primary).group.applicationTarget
        return groups.filter { it.type == 130 }.map { group ->
            val decision = groupDecision(group) ?: if (group.applicationId != parent) Decision(SelectedRejection.ParentMismatch, "aoc_parent_mismatch") else null
            SelectedGroupCandidate(group, decision?.rejection, decision?.predicate, groups.indexOf(group))
        }
    }

    fun select(primary: SelectedProgramChoice, update: SelectedProgramChoice?, addOnIds: List<String>): SelectedContentSelection {
        val chosen = requirePrimary(primary)
        require(chosen.group.type in setOf(128, 129)) { "selected_primary_role" }
        if (update != null) require(updates(primary).any { it.program.choice == update.checked() && it.rejection == null }) { "selected_update_not_available" }
        require(addOnIds.all { id -> addOns(primary).any { it.group.id == id && it.rejection == null } }) { "selected_aoc_not_available" }
        return SelectedContentSelection.create(if (chosen.group.type == 128) SelectedContentMode.Application else SelectedContentMode.StandalonePatch,
            profile, requireNotNull(chosen.group.applicationTarget), primary, update, addOnIds)
    }

    companion object {
        const val MaxJsonChars = 4 * 1024 * 1024
        private const val MaxGroups = 512
        private const val MaxNcas = 2048
        private const val MaxReferences = 8192

        fun parse(text: String, expectedProfile: SelectedContentProfile): SelectedContentCatalog {
            checkJsonEnvelope(text, MaxJsonChars, 12)
            try {
                val root = Json.parseToJsonElement(text).objectValue()
                require(root.int("schema_version") == 1 && root.text("catalog_kind") == "readonly_multi_title_nsp")
                val profile = SelectedContentProfile.parse(root.text("content_info_profile"))
                require(profile == expectedProfile)
                var references = 0
                val ncas = root.array("ncas", MaxNcas).map { raw ->
                    val nca = raw.objectValue()
                    SelectedCatalogNca(nca.text("id", 1100), nca.unsigned("size")!!, nca.optionalInt("content_type"),
                        nca.optionalId("program_id"), nca.text("support"), nca.array("sections", 4).map {
                            val section = it.objectValue()
                            SelectedCatalogSection(section.int("fs_type"), section.int("hash_type"), section.bool("requires_base"),
                                section.bool("sparse"), section.bool("compressed"), section.int("encryption_type"))
                        }, nca.issues())
                }
                require(ncas.map { it.id }.distinct().size == ncas.size)
                val groups = root.array("groups", MaxGroups).map { raw ->
                    val group = raw.objectValue(); val header = group.obj("packaged_header"); val layout = group.obj("layout")
                    require(layout.text("content_info_profile") == profile.wire)
                    val contents = group.array("contents", 256).map {
                        require(++references <= MaxReferences)
                        val ref = it.objectValue()
                        selectedHex(ref.text("expected_sha256"), 64)
                        SelectedCatalogReference(selectedHex(ref.text("content_id"), 32), ref.int("type"),
                            ref.int("id_offset").also { index -> require(index in 0..255) }, ref.unsigned("decoded_size"),
                            ref.unsigned("actual_nca_size"), ref.optionalInt("actual_nca_type"), ref.optionalText("member", 1100),
                            ref.array("candidate_members", MaxNcas).map { candidate -> candidate.textValue(1100) },
                            ref.text("reference_consistency"), ref.optionalInt("attributes"), ref.text("hash_verification"))
                    }
                    val id = selectedGroupId(group.text("id"))
                    val programs = group.array("programs", 256).map {
                        val program = it.objectValue(); val content = selectedHex(program.text("content_id"), 32)
                        val offset = program.int("id_offset").also { index -> require(index in 0..255) }
                        val index = program.optionalInt("program_index"); val programId = program.optionalId("program_id")
                        val choice = if (index == offset && content.any { c -> c != '0' } && programId != null && programId.any { c -> c != '0' } &&
                            program.text("index_evidence") == "cnmt_program_id_offset") SelectedProgramChoice(id, content, programId, index) else null
                        SelectedCatalogProgram(choice, content, offset, program.bool("requires_base"))
                    }
                    val metas = group.array("content_meta_references", 256).map {
                        require(++references <= MaxReferences)
                        val meta = it.objectValue()
                        meta.text("state") == "resolved" && meta.array("candidate_groups", MaxGroups).size == 1
                    }
                    SelectedCatalogGroup(id, group.optionalId("title_id"), group.optionalLong("version")?.also { require(it in 0..0xffffffffL) },
                        group.optionalInt("type"), group.optionalId("explicit_application_id"), group.optionalId("patch_id"),
                        group.optionalId("data_patch_id"), group.text("parsing"), group.text("cnmt_trailer_digest_observation"),
                        header.optionalInt("install_type"), header.optionalInt("platform"), header.optionalInt("attributes"),
                        header.optionalInt("storage_id"), header.optionalInt("committed_raw"), layout.text("extended_data_support"),
                        contents, programs, metas.all { it }, group.issues())
                }
                require(groups.map { it.id }.distinct().size == groups.size)
                val complete = root.bool("bounded_scan_complete") && root.bool("all_metadata_enumerated") &&
                    groups.all { it.parsing == "parsed" && "metadata_ambiguity" !in it.issues } &&
                    ncas.all { it.type != null && it.type in 0..5 && "duplicate_content_id" !in it.issues }
                return SelectedContentCatalog(profile, groups, ncas.associateBy { it.id }, complete, root.text("inspector") == "pine_existing_nca")
            } catch (error: Exception) {
                throw IllegalArgumentException("selected_catalog_invalid")
            }
        }
    }
}

private fun JsonElement.objectValue() = this as? JsonObject ?: throw IllegalArgumentException("selected_catalog_object")
private fun JsonElement.textValue(maximum: Int = 256): String {
    val value = this as? JsonPrimitive ?: throw IllegalArgumentException("selected_catalog_text")
    require(value.isString && value.content.length <= maximum)
    return value.content
}
private fun JsonObject.text(key: String, maximum: Int = 256) = getValue(key).textValue(maximum)
private fun JsonObject.optionalText(key: String, maximum: Int = 256) = getValue(key).takeUnless { it == JsonNull }?.textValue(maximum)
private fun JsonObject.optionalId(key: String) = optionalText(key)?.let { selectedHex(it, 16) }
private fun JsonObject.obj(key: String) = getValue(key).objectValue()
private fun JsonObject.array(key: String, maximum: Int): JsonArray = (getValue(key) as? JsonArray
    ?: throw IllegalArgumentException("selected_catalog_array")).also { require(it.size <= maximum) }
private fun JsonObject.bool(key: String): Boolean {
    val primitive = getValue(key) as? JsonPrimitive ?: error("boolean")
    require(!primitive.isString); return primitive.booleanOrNull ?: error("boolean")
}
private fun JsonObject.optionalLong(key: String): Long? = getValue(key).takeUnless { it == JsonNull }?.let {
    val primitive = it as? JsonPrimitive ?: error("integer")
    require(!primitive.isString); primitive.content.toLongOrNull() ?: error("integer")
}
private fun JsonObject.optionalInt(key: String): Int? = optionalLong(key)?.also { require(it in Int.MIN_VALUE..Int.MAX_VALUE) }?.toInt()
private fun JsonObject.int(key: String) = requireNotNull(optionalInt(key))
private fun JsonObject.unsigned(key: String): ULong? = getValue(key).takeUnless { it == JsonNull }?.let {
    val primitive = it as? JsonPrimitive ?: error("unsigned")
    require(!primitive.isString); primitive.content.toULongOrNull() ?: error("unsigned")
}
private fun JsonObject.issues(): Set<String> = array("issues", 256).map { it.objectValue().text("code") }.toSet()
