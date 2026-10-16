// SPDX-License-Identifier: MPL-2.0
package emu.skyline.diagnostics

import java.io.File
import java.io.OutputStream
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

/** Deliberately flat allowlist: never traverse app storage or include arbitrary attachments. */
object DiagnosticExport {
    const val MAX_FILE_BYTES = 8 * 1024 * 1024
    private val uri = Regex("(?i)(?:content|file|https?)://[^\\s\"<>]+")
    private val path = Regex("/(?:storage|sdcard|data|mnt)/[^\\s\"<>]+")
    private val secret = Regex("""(?i)(?:prod\.keys|title\.keys|ticket|password|token|secret|credential)(?:[=: ]+[^\s"{}]*)?""")
    private val secretProperty = Regex("""(?i)("(?:password|token|secret|credential|ticket)"\s*:\s*)"(?:\\.|[^"\\])*"""")
    fun redact(text: String): String {
        val properties = secretProperty.replace(text) { it.groupValues[1] + "\"[REDACTED_SECRET]\"" }
        return secret.replace(path.replace(uri.replace(properties, "[REDACTED_URI]"), "[REDACTED_PATH]"), "[REDACTED_SECRET]")
    }
    fun files(directory: File, textLog: File): List<File> = (0..7).map { File(directory, "diagnostics-$it.jsonl") }
        .plus(textLog).filter { it.isFile && it.canonicalFile.parentFile == it.parentFile.canonicalFile && it.length() <= MAX_FILE_BYTES }
    fun write(output: OutputStream, files: List<File>, metadata: String) {
        ZipOutputStream(output).use { zip ->
            zip.putNextEntry(ZipEntry("session-metadata.txt"))
            zip.write(redact(metadata).toByteArray(Charsets.UTF_8)); zip.closeEntry()
            for (file in files) {
                require(file.canonicalFile == file.absoluteFile && file.isFile && file.length() <= MAX_FILE_BYTES)
                require(file.name == "emulation.log" || Regex("diagnostics-[0-7]\\.jsonl").matches(file.name))
                zip.putNextEntry(ZipEntry(file.name))
                // Bound snapshot memory and avoid copying newly appended data beyond the cap.
                val bytes = file.inputStream().use { input ->
                    val buffer = java.io.ByteArrayOutputStream()
                    val chunk = ByteArray(8192)
                    var remaining = MAX_FILE_BYTES
                    while (remaining > 0) {
                        val read = input.read(chunk, 0, minOf(chunk.size, remaining))
                        if (read < 0) break
                        buffer.write(chunk, 0, read); remaining -= read
                    }
                    buffer.toByteArray()
                }
                zip.write(redact(bytes.toString(Charsets.UTF_8)).toByteArray(Charsets.UTF_8))
                zip.closeEntry()
            }
        }
    }
}
