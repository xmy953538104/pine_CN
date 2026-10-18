// SPDX-License-Identifier: MPL-2.0
package emu.skyline.emulation

import android.net.Uri
import android.content.Intent
import android.os.ParcelFileDescriptor
import android.os.Bundle
import emu.skyline.BuildConfig
import emu.skyline.data.BaseAppItem
import emu.skyline.data.AppItemTag
import emu.skyline.loader.SelectedContentBridge
import emu.skyline.loader.SelectedContentSelection
import emu.skyline.loader.SelectedContentIntentTag

internal fun selectedContentIsAvailable(): Boolean = BuildConfig.SELECTED_CONTENT_ENABLED &&
    try { SelectedContentBridge.isAvailable() } catch (error: LinkageError) { false }

/** Presence is deliberate: a malformed/present extra must never mean ordinary playback. */
internal fun selectedLaunchSelection(present: Boolean, payload: String?, available: () -> Boolean): SelectedContentSelection? {
    if (!present) return null
    val selection = SelectedContentSelection.decode(requireNotNull(payload) { "selected_payload_missing" })
    require(available()) { "selected_entry_unavailable" }
    return selection
}

/** The explicit selected route ignores saved legacy Update/DLC associations. */
internal fun openLaunchContentFiles(base: Uri, selection: SelectedContentSelection?,
    dlcs: () -> List<Uri>, update: () -> Uri, open: (Uri) -> ParcelFileDescriptor?): ContentFiles =
    if (selection == null) openContentFiles(base, dlcs(), update(), open)
    else openContentFiles(base, emptyList(), Uri.EMPTY, open)

/** Shared by the Activity and JVM tests: no error or unsupported-selection fallback. */
internal fun dispatchContentLaunch(selection: SelectedContentSelection?, ordinary: () -> Unit,
    selected: (SelectedContentSelection) -> Unit) {
    if (selection == null) ordinary() else selected(selection)
}

internal fun selectedContentIntentChanged(previous: Intent, next: Intent): Boolean =
    previous.hasExtra(SelectedContentIntentTag) != next.hasExtra(SelectedContentIntentTag) ||
        runCatching { previous.getStringExtra(SelectedContentIntentTag) }.getOrNull() !=
        runCatching { next.getStringExtra(SelectedContentIntentTag) }.getOrNull()

/** Settings use the explicit application identity, never the placeholder's URI key or save owner. */
internal fun selectedSettingsSelection(extras: Bundle?): SelectedContentSelection? {
    if (extras?.containsKey(SelectedContentIntentTag) != true) return null
    return SelectedContentSelection.decode(requireNotNull(extras.getString(SelectedContentIntentTag)) {
        "selected_payload_missing"
    })
}

internal fun contentSettingsIdentity(item: BaseAppItem, selection: SelectedContentSelection?): String =
    selection?.applicationId ?: item.titleId ?: item.key()

internal fun contentSettingsArguments(item: BaseAppItem, selection: SelectedContentSelection?) = Bundle().apply {
    putSerializable(AppItemTag, item)
    selection?.let { putString(SelectedContentIntentTag, it.encode()) }
}
