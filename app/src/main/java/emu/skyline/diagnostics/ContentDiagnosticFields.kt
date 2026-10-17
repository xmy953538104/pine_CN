// SPDX-License-Identifier: MPL-2.0
package emu.skyline.diagnostics

import emu.skyline.loader.DiagnosticContentIdentity

/** Facts from native metadata, separate from legacy save-owner-based AppEntry selection. */
internal object ContentDiagnosticFields {
    private fun identity(value: DiagnosticContentIdentity?, legacy: String?) = linkedMapOf(
        "title_id" to value?.contentTitleId.orEmpty(), "application_title_id" to DiagnosticContentIdentity.id(value?.applicationId),
        "cnmt_title_id" to DiagnosticContentIdentity.id(value?.cnmtId), "program_id" to DiagnosticContentIdentity.id(value?.programId),
        "save_data_owner_id" to DiagnosticContentIdentity.id(value?.saveOwnerId), "program_nca_id" to value?.programContentId.orEmpty(),
        "content_title_id_source" to (value?.contentTitleSource ?: "unknown"), "application_identity_source" to value?.applicationSource.orEmpty(),
        "identity_observation" to (value?.observation ?: "not_observed_in_this_process"),
        "cnmt_version" to value?.cnmtVersion.orEmpty(), "legacy_app_entry_title_id" to Diagnostics.normalizedTitleId(legacy))
    fun patchSelection(owner: DiagnosticContentIdentity?, legacyOwner: String?, selected: DiagnosticContentIdentity?, legacySelected: String?,
        entry: String, policy: String) = identity(owner, legacyOwner) + mapOf(
        "phase" to "ui_association", "execution_confirmed" to "false", "entry" to entry, "policy" to policy,
        "selected_title_id" to selected?.contentTitleId.orEmpty(), "selected_cnmt_title_id" to DiagnosticContentIdentity.id(selected?.cnmtId),
        "selected_program_id" to DiagnosticContentIdentity.id(selected?.programId), "selected_save_data_owner_id" to DiagnosticContentIdentity.id(selected?.saveOwnerId),
        "selected_identity_observation" to (selected?.observation ?: "not_observed_in_this_process"),
        "legacy_selected_app_entry_title_id" to Diagnostics.normalizedTitleId(legacySelected))
    fun association(owner: DiagnosticContentIdentity?, legacyTitle: String?, updates: List<DiagnosticContentIdentity?>, dlcs: List<DiagnosticContentIdentity?>) =
        identity(owner, legacyTitle) + mapOf(
            "metadata_status" to if (owner?.contentTitleId.isNullOrEmpty()) "unavailable" else owner!!.observation,
            "update_count" to updates.size.toString(), "dlc_count" to dlcs.size.toString(),
            "update_title_ids" to updates.take(8).joinToString(",") { it?.contentTitleId.orEmpty() },
            "dlc_title_ids" to dlcs.take(8).joinToString(",") { it?.contentTitleId.orEmpty() },
            "id_list_scope" to "first_8_per_kind_unknown_entries_empty", "policy" to "legacy_owner_association_all_enabled_first_update")
    fun imported(value: DiagnosticContentIdentity?, legacyTitle: String?, legacyParent: String?, owner: DiagnosticContentIdentity?, legacyOwner: String?,
        version: String?, type: String, expected: String, result: String, success: Boolean, duplicate: Boolean,
        typeMatches: Boolean, parentMatches: Boolean) = identity(value, legacyTitle) + mapOf(
        "parent_title_id" to DiagnosticContentIdentity.id(value?.parentId), "display_version" to version.orEmpty(),
        "actual_type" to type, "expected_type" to expected, "loader_result" to result,
        "metadata_status" to if (success && !value?.contentTitleId.isNullOrEmpty()) value!!.observation else "unavailable",
        "metadata_unknown_reason" to when { !success -> "loader_result_not_success"; value?.contentTitleId.isNullOrEmpty() -> "content_ids_not_supplied"; else -> "" },
        "duplicate" to duplicate.toString(), "parent_match" to parentMatches.toString(),
        "expected_application_title_id" to DiagnosticContentIdentity.id(owner?.applicationId),
        "legacy_parent_title_id" to Diagnostics.normalizedTitleId(legacyParent), "legacy_owner_title_id" to Diagnostics.normalizedTitleId(legacyOwner),
        "decision" to when { duplicate -> "duplicate_reported_success"; success && typeMatches && parentMatches -> "eligible_for_registration"; else -> "reject" })
}
