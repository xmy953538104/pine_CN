// SPDX-License-Identifier: MPL-2.0
package emu.skyline.diagnostics

import android.app.Application
import android.net.Uri
import emu.skyline.data.AppItem
import emu.skyline.data.BaseAppItem
import emu.skyline.data.selectUriContent
import emu.skyline.loader.AppEntry
import emu.skyline.loader.LoaderResult
import emu.skyline.loader.RomFormat
import emu.skyline.loader.RomType
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import java.io.Serializable

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, manifest = Config.NONE, sdk = [29])
class ContentSelectionTest {
    private fun entry(id: String, parent: String? = "base", type: RomType = RomType.Update, version: String = "1.0") = AppEntry(
        name = id, version = version, titleId = id, addOnContentBaseId = null,
        author = null, icon = null, romType = type, parentTitleId = parent,
        format = RomFormat.NSP, uri = Uri.parse("content://fixture/$id.nsp"), loaderResult = LoaderResult.Success
    )

    @Test fun appItemUsesFirstEnabledUpdateAndMatchingDisplayedVersion() {
        val disabled = BaseAppItem(entry("disabled", version = "0.1"), false)
        val first = BaseAppItem(entry("first", version = "2.0"), true)
        val later = BaseAppItem(entry("later", version = "9.0"), true)
        val app = AppItem(entry("base", type = RomType.Base, version = "1.0"), listOf(disabled, first, later), emptyList())
        assertSame(first, app.getEnabledUpdate())
        assertEquals("2.0", app.getVersion())
    }

    @Test fun appItemWithNoEnabledUpdateKeepsBaseVersionAndEnabledDlcOrder() {
        val first = BaseAppItem(entry("dlc-first", type = RomType.DLC), true)
        val disabled = BaseAppItem(entry("dlc-off", type = RomType.DLC), false)
        val later = BaseAppItem(entry("dlc-later", type = RomType.DLC), true)
        val app = AppItem(entry("base", type = RomType.Base, version = "1.0"), listOf(BaseAppItem(entry("update"), false)), listOf(first, disabled, later))
        assertNull(app.getEnabledUpdate())
        assertEquals("1.0", app.getVersion())
        assertEquals(listOf(first, later), app.getEnabledDlcs())
    }

    @Test fun uriFallbackUsesLastMatchingUpdateAndPreservesAllMatchingDlcOrder() {
        val first = entry("first")
        val later = entry("later")
        val dlcFirst = entry("dlc-first", type = RomType.DLC)
        val dlcLater = entry("dlc-later", type = RomType.DLC)
        val foreign = entry("foreign-last", parent = "other")
        val selection = selectUriContent("base", listOf<Serializable>(first, dlcFirst, entry("base-file", type = RomType.Base), later, dlcLater, foreign))
        assertEquals(later.uri, selection.updateUri)
        assertEquals(later.titleId, selection.updateTitleId)
        assertEquals(listOf(dlcFirst.uri, dlcLater.uri), selection.dlcUris)
        val ui = AppItem(entry("base", type = RomType.Base), listOf(BaseAppItem(first, true), BaseAppItem(later, true)), emptyList())
        assertEquals(first.uri, ui.getEnabledUpdate()!!.uri)
    }

    @Test fun uriFallbackWithoutMatchingUpdateKeepsEmptyUriAndNullParentMatching() {
        val missing = selectUriContent("base", listOf(entry("foreign", parent = "other")))
        assertEquals(Uri.EMPTY, missing.updateUri)
        assertNull(missing.updateTitleId)
        assertTrue(missing.dlcUris.isEmpty())
        val nullParent = entry("null-parent", parent = null)
        assertEquals(nullParent.uri, selectUriContent(null, listOf(nullParent)).updateUri)
    }
}
