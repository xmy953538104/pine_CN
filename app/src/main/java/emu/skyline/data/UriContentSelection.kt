// SPDX-License-Identifier: MPL-2.0
package emu.skyline.data

import android.net.Uri
import emu.skyline.diagnostics.Diagnostics
import emu.skyline.loader.AppEntry
import emu.skyline.loader.RomType
import java.io.Serializable

/** URI launches deliberately keep the existing last-matching-update policy. */
data class UriContentSelection(val updateUri: Uri, val updateTitleId: String?, val dlcUris: List<Uri>)

fun selectUriContent(baseTitleId: String?, contents: List<Serializable>): UriContentSelection {
    var updateUri = Uri.EMPTY
    var selectedUpdateTitle: String? = null
    val dlcUris = ArrayList<Uri>()
    contents.filter { entry ->
        (entry as AppEntry).parentTitleId == baseTitleId
    }.forEach { entry ->
        (entry as AppEntry).uri?.let { uri ->
            if (entry.romType == RomType.DLC) dlcUris.add(uri)
            if (entry.romType == RomType.Update) {
                Diagnostics.event("content", "PATCH_CANDIDATE") { "entry=uri_fallback policy=last_match title_id=${entry.titleId} overwrites=${updateUri != Uri.EMPTY}" }
                selectedUpdateTitle = entry.titleId
                updateUri = uri
            }
        }
    }
    return UriContentSelection(updateUri, selectedUpdateTitle, dlcUris)
}
