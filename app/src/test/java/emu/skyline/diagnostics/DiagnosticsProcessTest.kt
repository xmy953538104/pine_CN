// SPDX-License-Identifier: MPL-2.0
package emu.skyline.diagnostics

import android.app.Application
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import org.robolectric.shadows.ShadowApplication
import java.io.File
import javax.xml.parsers.DocumentBuilderFactory
import org.w3c.dom.Element

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, manifest = Config.NONE, sdk = [29])
class DiagnosticsProcessTest {
    @Test fun settingsSearchCanTraverseDiagnosticsEntryAsCategory() {
        // SettingsActivity's search traverses every root item as a category.
        // A standalone diagnostic Preference here used to crash that search.
        val source = File("src/main/res/xml/app_preferences.xml")
        assertTrue("Run this test with the app Gradle unit-test task", source.isFile)
        val document = DocumentBuilderFactory.newInstance().apply { isNamespaceAware = true }
            .newDocumentBuilder().parse(source)
        var foundEntry = false
        val categories = document.documentElement.childNodes
        for (index in 0 until categories.length) {
            val category = categories.item(index) as? Element ?: continue
            assertEquals("PreferenceCategory", category.tagName)
            val entries = category.childNodes
            for (entryIndex in 0 until entries.length) {
                val entry = entries.item(entryIndex) as? Element ?: continue
                if (entry.getAttributeNS("http://schemas.android.com/apk/res/android", "key") == "developer_diagnostics") {
                    foundEntry = true
                }
            }
        }
        assertTrue("Diagnostics entry must be reachable by settings search", foundEntry)
    }

    @Test fun androidProcessNamesUseSeparateFixedWriterDirectories() {
        val context = RuntimeEnvironment.getApplication()
        ShadowApplication.setProcessName("io.github.pine.emu")
        val main = Diagnostics.directory(context)
        assertEquals("main", Diagnostics.processScope)
        ShadowApplication.setProcessName("io.github.pine.emu:emulationProcess")
        val emulation = Diagnostics.directory(context)
        assertEquals("emulation", Diagnostics.processScope)
        assertEquals("main", main.name)
        assertEquals("emulation", emulation.name)
        assertEquals(Diagnostics.rootDirectory(context), main.parentFile)
        assertEquals(main.parentFile, emulation.parentFile)
        assertNotEquals(main, emulation)
    }

    @Test fun sessionMarkersAreReadFromBothScopesWithoutPreferenceCache() {
        val context = RuntimeEnvironment.getApplication()
        val root = Diagnostics.rootDirectory(context)
        val main = "11111111-1111-1111-1111-111111111111"
        val oldEmulation = "22222222-2222-2222-2222-222222222222"
        val newEmulation = "33333333-3333-3333-3333-333333333333"
        File(root, "main/session-id.txt").apply { parentFile!!.mkdirs(); writeText(main) }
        val emulation = File(root, "emulation/session-id.txt").apply { parentFile!!.mkdirs(); writeText(oldEmulation) }
        assertEquals(mapOf("main" to main, "emulation" to oldEmulation), Diagnostics.sessions(context))
        emulation.writeText(newEmulation)
        assertEquals(mapOf("main" to main, "emulation" to newEmulation), Diagnostics.sessions(context))
    }
}
