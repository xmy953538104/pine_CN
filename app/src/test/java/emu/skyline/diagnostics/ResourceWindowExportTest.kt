// SPDX-License-Identifier: MPL-2.0
package emu.skyline.diagnostics

import kotlinx.serialization.json.*
import org.junit.Assert.*
import org.junit.Test
import java.io.ByteArrayOutputStream
import java.io.File
import java.nio.file.Files
import java.nio.file.attribute.PosixFilePermission
import java.util.zip.ZipInputStream

internal object ResourceWindowNativeBridge {
    init { System.load(requireNotNull(System.getProperty("pine.diagnostics.metadataFixture"))) }
    @JvmStatic external fun persist(directory: String, marker: Int, fault: Int): Array<String>
}

class ResourceWindowExportTest {
    private fun sandbox(action: (File, File, File, File) -> Unit) {
        val root = Files.createTempDirectory("pine-native-window-zip").toFile()
        val ancestor = File(root, "search-only").apply { mkdirs() }
        val logs = File(ancestor, "app-private/diagnostics").apply { mkdirs() }
        val cache = File(root, "cache").apply { mkdirs() }
        val text = File(root, "emulation.log")
        try { action(ancestor, logs, text, cache) }
        finally {
            Files.setPosixFilePermissions(ancestor.toPath(), setOf(PosixFilePermission.OWNER_READ, PosixFilePermission.OWNER_WRITE, PosixFilePermission.OWNER_EXECUTE))
            root.deleteRecursively()
        }
    }
    private fun archive(snapshot: DiagnosticExport.Snapshot): Map<String, String> {
        val bytes = ByteArrayOutputStream().also { DiagnosticExport.write(it, snapshot) }.toByteArray()
        val result = linkedMapOf<String, String>()
        ZipInputStream(bytes.inputStream()).use { zip ->
            while (true) { val entry = zip.nextEntry ?: break; result[entry.name] = zip.readBytes().toString(Charsets.UTF_8) }
        }
        return result
    }
    private fun rows(entries: Map<String, String>, name: String) = entries.getValue(name).lineSequence()
        .filter { it.isNotBlank() }.map { Json.parseToJsonElement(it).jsonObject }.toList()

    @Test fun searchOnlyAncestorActualNativeWindowReachesRealRedactedZip() = sandbox { ancestor, logs, text, cache ->
        val main = File(logs, "main").apply { mkdirs() }
        Files.setPosixFilePermissions(ancestor.toPath(), setOf(PosixFilePermission.OWNER_EXECUTE,
            PosixFilePermission.GROUP_EXECUTE, PosixFilePermission.OTHERS_EXECUTE))
        assertArrayEquals(arrayOf("true", "none", "none", "none"), ResourceWindowNativeBridge.persist(main.path, 0, 0))
        assertTrue(File(main, "resource-window.jsonl").length() in 1L..131072L)
        DiagnosticExport.capture(logs, text, cache, "schema=1").use { snapshot ->
            val entries = archive(snapshot)
            val values = rows(entries, "main/resource-window.jsonl")
            assertEquals("resource_window_metadata", values.first().getValue("record_kind").jsonPrimitive.content)
            assertEquals("7", values.last().getValue("fields").jsonObject.getValue("present_accepted").jsonPrimitive.content)
            assertFalse(entries.values.any { it.contains("CONTROLLED_OMIT") || it.contains("/data/user") })
        }
    }

    @Test fun realWriteAndRenameFaultsPreserveOldFileThenRecoveredWindowIsExported() = sandbox { _, logs, text, cache ->
        val main = File(logs, "main").apply { mkdirs() }
        assertEquals("true", ResourceWindowNativeBridge.persist(main.path, 0, 0).first())
        val target = File(main, "resource-window.jsonl")
        val original = target.readBytes()
        for ((fault, stage, code) in listOf(Triple(1, "write_metadata", "ENOSPC"), Triple(2, "rename", "EACCES"))) {
            assertArrayEquals(arrayOf("false", stage, "syscall_error", code), ResourceWindowNativeBridge.persist(main.path, 1, fault))
            assertArrayEquals(original, target.readBytes())
            assertFalse(File(main, "resource-window.tmp").exists())
        }
        assertArrayEquals(arrayOf("true", "none", "none", "none"), ResourceWindowNativeBridge.persist(main.path, 1, 3))
        DiagnosticExport.capture(logs, text, cache, "schema=1").use { snapshot ->
            val entries = archive(snapshot)
            val record = rows(entries, "main/resource-window.jsonl").last()
            assertEquals("after", record.getValue("fields").jsonObject.getValue("marker").jsonPrimitive.content)
            assertFalse(entries.keys.any { it.endsWith(".tmp") })
        }
    }

    @Test fun stalePartialTemporaryRecoversBeforeActualWindowZipCapture() = sandbox { _, logs, text, cache ->
        val emulation = File(logs, "emulation").apply { mkdirs() }
        File(emulation, "resource-window.tmp").writeText("controlled incomplete write")
        assertEquals("true", ResourceWindowNativeBridge.persist(emulation.path, 1, 0).first())
        assertFalse(File(emulation, "resource-window.tmp").exists())
        DiagnosticExport.capture(logs, text, cache, "schema=1").use { snapshot ->
            val entries = archive(snapshot)
            assertEquals("after", rows(entries, "emulation/resource-window.jsonl").last()
                .getValue("fields").jsonObject.getValue("marker").jsonPrimitive.content)
        }
    }
}
