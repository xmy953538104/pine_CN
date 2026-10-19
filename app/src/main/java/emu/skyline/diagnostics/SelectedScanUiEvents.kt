// SPDX-License-Identifier: MPL-2.0
package emu.skyline.diagnostics

import android.content.Context
import emu.skyline.R
import emu.skyline.data.BaseAppItem

/** Records the resource actually rendered by the ordinary version fallback, without implying a scan. */
internal fun appDetailsVersion(context: Context, item: BaseAppItem, version: String?): String {
    if (version != null) return version
    val resource = item.loaderResultResource()
    val name = when (resource) {
        R.string.metadata_missing -> "metadata_missing"
        R.string.invalid_file -> "invalid_file"
        R.string.missing_title_key -> "missing_title_key"
        R.string.incomplete_prod_keys -> "incomplete_prod_keys"
        R.string.error_sparse_nca -> "error_sparse_nca"
        R.string.error_compressed_nca -> "error_compressed_nca"
        else -> "unknown_ui_resource"
    }
    Diagnostics.fieldsEvent("content", "APP_DETAILS_METADATA_HINT_SHOWN") {
        mapOf("ui_resource_name" to name, "ui_stage" to "app_details_version", "scan_performed" to "false",
            "loader_result" to item.loaderResult.name)
    }
    return context.getString(resource)
}
