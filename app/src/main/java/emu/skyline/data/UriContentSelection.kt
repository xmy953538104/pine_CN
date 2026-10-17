// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)
package emu.skyline.data

import android.net.Uri
import emu.skyline.diagnostics.Diagnostics
import emu.skyline.loader.AppEntry
import emu.skyline.loader.RomType
import emu.skyline.loader.diagnosticIdentity
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
                Diagnostics.fieldsEvent("content", "PATCH_CANDIDATE") {
                    emu.skyline.diagnostics.ContentDiagnosticFields.patchSelection(null, baseTitleId, entry.diagnosticIdentity,
                        entry.titleId, "uri_fallback", "last_match") + mapOf("overwrites" to (updateUri != Uri.EMPTY).toString())
                }
                selectedUpdateTitle = entry.titleId
                updateUri = uri
            }
        }
    }
    return UriContentSelection(updateUri, selectedUpdateTitle, dlcUris)
}
