/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)
 */

package emu.skyline.data

import android.content.Context
import android.graphics.Bitmap
import androidx.core.content.ContextCompat
import androidx.core.graphics.drawable.toBitmap
import emu.skyline.BuildConfig
import emu.skyline.R
import emu.skyline.SkylineApplication
import emu.skyline.loader.AppEntry
import emu.skyline.loader.LoaderResult
import java.io.Serializable

/**
 * This class is a wrapper around [AppEntry], it is used for passing around game metadata
 */
@Suppress("SERIAL")
class AppItem(meta : AppEntry, private val updates : List<BaseAppItem>, private val dlcs : List<BaseAppItem>) : BaseAppItem(meta), Serializable {

    fun getEnabledDlcs() : List<BaseAppItem> {
        return dlcs.filter { it.enabled }
    }

    fun getEnabledUpdate() : BaseAppItem? {
        val selected = updates.firstOrNull { it.enabled }
        emu.skyline.diagnostics.Diagnostics.event("content", "PATCH_SELECTED") { "entry=AppItem policy=first_enabled title_id=$titleId candidates=${updates.size} enabled=${updates.count { it.enabled }} selected_title_id=${selected?.titleId} version=${selected?.titleVersion}" }
        return selected
    }

    fun getVersion() : String? {
        return updates.firstOrNull { it.enabled }?.titleVersion ?: titleVersion
    }
}
