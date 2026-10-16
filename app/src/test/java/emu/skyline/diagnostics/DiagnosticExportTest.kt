// SPDX-License-Identifier: MPL-2.0
package emu.skyline.diagnostics

import org.junit.Assert.*
import org.junit.Test
import java.io.ByteArrayOutputStream
import java.nio.file.Files
import java.util.zip.ZipInputStream

class DiagnosticExportTest {
    @Test fun redactionKeepsJsonParseable() {
        val source = """{"schema":1,"message":"世界 content://private/rom","token":"do-not-leak","path":"/data/private/rom"}"""
        val redacted = DiagnosticExport.redact(source)
        kotlinx.serialization.json.Json.parseToJsonElement(redacted)
        assertFalse(redacted.contains("do-not-leak"))
        assertFalse(redacted.contains("private"))
    }

    @Test fun redactsUrisPathsAndCredentials() {
        val text = DiagnosticExport.redact("ROM content://authority/private/game.nsp /storage/emulated/0/private password=abc 世界")
        assertFalse(text.contains("authority")); assertFalse(text.contains("game.nsp"))
        assertFalse(text.contains("emulated")); assertFalse(text.contains("abc"))
        assertTrue(text.contains("世界"))
    }
    @Test fun exportIncludesOnlyFlatAllowlistAndUtf8() {
        val root = Files.createTempDirectory("pine-diag-test").toFile()
        try {
            val logs = java.io.File(root, "diagnostics").apply { mkdir() }
            java.io.File(logs, "diagnostics-0.jsonl").writeText("{\"message\":\"世界 content://private/rom\"}\n")
            java.io.File(logs, "prod.keys").writeText("DO_NOT_EXPORT")
            java.io.File(logs, "subdirectory").mkdir()
            java.io.File(logs, "subdirectory/diagnostics-1.jsonl").writeText("DO_NOT_EXPORT")
            val log = java.io.File(root, "emulation.log").apply { writeText("content://private/game.nsp") }
            val files = DiagnosticExport.files(logs, log)
            assertEquals(2, files.size)
            val output = ByteArrayOutputStream()
            DiagnosticExport.write(output, files, "schema=1")
            val names = mutableListOf<String>()
            ZipInputStream(output.toByteArray().inputStream()).use { zip ->
                while (true) {
                    val entry = zip.nextEntry ?: break
                    names.add(entry.name)
                    val text = zip.readBytes().toString(Charsets.UTF_8)
                    assertFalse(text.contains("private")); assertFalse(text.contains("DO_NOT_EXPORT"))
                    if (entry.name.endsWith("jsonl")) assertTrue(text.contains("世界"))
                }
            }
            assertEquals(setOf("session-metadata.txt", "diagnostics-0.jsonl", "emulation.log"), names.toSet())
        } finally { root.deleteRecursively() }
    }
    @Test fun rejectsSymlinksAndOversizedFiles() {
        val root = Files.createTempDirectory("pine-diag-test").toFile()
        try {
            val outside = java.io.File(root, "private").apply { mkdir() }
            val source = java.io.File(outside, "secret").apply { writeText("private") }
            Files.createSymbolicLink(java.io.File(root, "diagnostics-0.jsonl").toPath(), source.toPath())
            val large = java.io.File(root, "diagnostics-1.jsonl")
            java.io.RandomAccessFile(large, "rw").use { it.setLength(DiagnosticExport.MAX_FILE_BYTES.toLong() + 1) }
            assertTrue(DiagnosticExport.files(root, java.io.File(root, "emulation.log")).isEmpty())
        } finally { root.deleteRecursively() }
    }
}
