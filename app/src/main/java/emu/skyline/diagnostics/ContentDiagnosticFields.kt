// SPDX-License-Identifier: MPL-2.0
package emu.skyline.diagnostics

/** Facts at existing UI decisions. No URI, filename or selection-policy inference. */
internal object ContentDiagnosticFields {
    fun association(title: String?, updates: List<String?>, dlcs: List<String?>) = mapOf(
        "title_id" to Diagnostics.normalizedTitleId(title), "application_title_id" to Diagnostics.normalizedTitleId(title),
        "metadata_status" to if (Diagnostics.normalizedTitleId(title).isEmpty()) "title_unavailable" else "available",
        "update_count" to updates.size.toString(), "dlc_count" to dlcs.size.toString(),
        "update_title_ids" to updates.take(8).map(Diagnostics::normalizedTitleId).joinToString(","),
        "dlc_title_ids" to dlcs.take(8).map(Diagnostics::normalizedTitleId).joinToString(","),
        "id_list_scope" to "first_8_per_kind", "policy" to "all_enabled_first_update")
    fun imported(title: String?, parent: String?, owner: String?, version: String?, type: String, expected: String,
        result: String, success: Boolean, duplicate: Boolean, typeMatches: Boolean, parentMatches: Boolean) = mapOf(
        "title_id" to Diagnostics.normalizedTitleId(title), "parent_title_id" to Diagnostics.normalizedTitleId(parent),
        "application_title_id" to Diagnostics.normalizedTitleId(owner), "display_version" to version.orEmpty(),
        "actual_type" to type, "expected_type" to expected, "loader_result" to result,
        "metadata_status" to if (success && Diagnostics.normalizedTitleId(title).isNotEmpty()) "available" else "unavailable",
        "metadata_unknown_reason" to when { !success -> "loader_result_not_success"; Diagnostics.normalizedTitleId(title).isEmpty() -> "title_not_supplied"; else -> "" },
        "duplicate" to duplicate.toString(), "parent_match" to parentMatches.toString(),
        "decision" to when { duplicate -> "duplicate_reported_success"; success && typeMatches && parentMatches -> "eligible_for_registration"; else -> "reject" })
}
