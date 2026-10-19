// SPDX-License-Identifier: MPL-2.0
package emu.skyline.loader

import android.content.Context
import emu.skyline.R
import kotlinx.serialization.json.*

/** Presentation of the independently rebuilt report; never receives the raw Catalog or key files. */
internal object SelectedScanDetails {
    const val MaxTextChars = 16 * 1024

    fun summary(context: Context, report: SelectedNativeScanReport): String? {
        if (report.schemaVersion != 2) return null
        val state = report.safe.getValue("key_state").jsonObject
        return context.getString(R.string.selected_observation_summary,
            state.getValue("title_keys").jsonObject.text("presence"), state.text("loaded_unique_title_key_count"),
            state.getValue("prod_keys").jsonObject.text("presence"), state.text("recognized_prod_slot_count"))
    }

    fun text(context: Context, report: SelectedNativeScanReport): String? {
        if (report.schemaVersion != 2) return null
        val state = report.safe.getValue("key_state").jsonObject
        val lines = mutableListOf(context.getString(R.string.selected_observation_caveat),
            context.getString(R.string.selected_observation_load, state.text("status"), state.text("counts_partial")))
        listOf("title_keys" to R.string.selected_observation_title_keys, "prod_keys" to R.string.selected_observation_prod_keys).forEach { (field, label) ->
            val file = state.getValue(field).jsonObject
            lines.add(context.getString(R.string.selected_observation_file, context.getString(label),
                file.text("presence"), file.text("read"), file.text("load")))
        }
        lines.add(context.getString(R.string.selected_observation_counts, state.text("loaded_unique_title_key_count"), state.text("recognized_prod_slot_count")))
        val summary = report.safe["catalog_summary"] as? JsonObject
        if (summary != null) {
            lines.add(context.getString(R.string.selected_observation_scope, summary.text("container_size_bytes"), summary.text("full_content_hash")))
            summary.getValue("groups").jsonArray.forEach { raw ->
                val group = raw.jsonObject
                lines.add(context.getString(R.string.selected_observation_group, group.text("ordinal"), group.text("role"),
                    group.text("version"), group.text("title_id"), group.text("digest_observation")))
            }
            summary.getValue("members").jsonArray.forEach { raw ->
                val member = raw.jsonObject
                val lookup = context.getString(when (member.text("title_key_lookup")) {
                    "matched" -> R.string.selected_observation_lookup_matched
                    "not_matched" -> R.string.selected_observation_lookup_missing
                    "not_required" -> R.string.selected_observation_lookup_not_required
                    else -> R.string.selected_observation_lookup_unknown
                })
                lines.add(context.getString(R.string.selected_observation_member, member.text("ordinal"), member.text("nca_type"),
                    member.text("size_bytes"), member.text("content_id_hint"), member.text("readability"), member.text("support"), lookup,
                    member.text("full_hash_verification")))
            }
            lines.add(context.getString(R.string.selected_observation_omitted, summary.text("groups_omitted"), summary.text("members_omitted")))
        }
        val result = lines.joinToString("\n\n")
        return if (result.length <= MaxTextChars) result else result.take(MaxTextChars - 160) + "\n\n" + context.getString(R.string.selected_observation_truncated)
    }

    private fun JsonObject.text(field: String) = (get(field) as? JsonPrimitive)?.contentOrNull ?: "unknown"
}
