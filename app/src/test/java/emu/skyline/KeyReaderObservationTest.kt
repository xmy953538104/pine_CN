// SPDX-License-Identifier: MPL-2.0
package emu.skyline

import android.app.Application
import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.Shadows.shadowOf
import org.robolectric.annotation.Config
import org.robolectric.annotation.Implementation
import org.robolectric.annotation.Implements
import java.io.ByteArrayInputStream
import java.io.File

/** Characterizes the unchanged importer using public synthetic strings only.
 * The SDK document-authority gate is a test substitute; import, trimming,
 * validation, file replacement and cleanup are the actual KeyReader code.
 * This is not an Android device/permission test or a proposal to relax parsing.
 */
@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, manifest = Config.NONE, sdk = [29],
    shadows = [KeyReaderObservationTest.SyntheticDocuments::class])
class KeyReaderObservationTest {
    @Implements(DocumentsContract::class)
    class SyntheticDocuments {
        companion object {
            @JvmStatic @Implementation
            fun isDocumentUri(context: Context, uri: Uri): Boolean =
                uri.authority == "synthetic.keys" && uri.path == "/document/title"
        }
    }

    private val context: Context get() = RuntimeEnvironment.getApplication()
    private val uri = Uri.parse("content://synthetic.keys/document/title")
    private val directory: File get() = File(context.filesDir.canonicalFile, "keys")
    private val installed: File get() = File(directory, "title.keys")
    @Before fun clear() { directory.deleteRecursively() }
    @After fun cleanup() { directory.deleteRecursively() }
    private fun import(text: String): KeyReader.ImportResult {
        shadowOf(context.contentResolver).registerInputStream(uri, ByteArrayInputStream(text.toByteArray(Charsets.UTF_8)))
        return KeyReader.import(context, uri, KeyReader.KeyType.Title)
    }

    @Test fun actualImporterAcceptsEmptyAndCommentsOnlyFilesAsSuccess() {
        for (text in listOf("", "\n  \n; synthetic comment\n")) {
            assertEquals(KeyReader.ImportResult.Success, import(text))
            assertTrue(installed.isFile)
            assertEquals("", installed.readText())
            assertFalse(File(directory, "title.keys.tmp").exists())
        }
    }

    @Test fun hexStringsOfWrongWidthsAndEmptyPairsAreCurrentlyAccepted() {
        for (length in listOf(0, 1, 31, 32, 33, 64)) {
            val key = "a".repeat(length)
            val value = "b".repeat(length)
            assertEquals("synthetic width $length", KeyReader.ImportResult.Success, import("$key=$value\n"))
            assertEquals("$key=$value\n", installed.readText())
        }
    }

    @Test fun nonHexAtExactly32CharactersIsCurrentlyAccepted() {
        val badKey = "G".repeat(32)
        val badValue = "Z".repeat(32)
        assertEquals(KeyReader.ImportResult.Success, import("$badKey=$badValue\n"))
        assertEquals("$badKey=$badValue\n", installed.readText())
    }

    @Test fun aWrongWidthNonHexValueIsRejectedAndThePreviousFileSurvives() {
        val original = "0".repeat(32) + "=" + "1".repeat(32) + "\n"
        assertEquals(KeyReader.ImportResult.Success, import(original))
        assertEquals(KeyReader.ImportResult.InvalidKeys, import("G".repeat(31) + "=" + "2".repeat(32) + "\n"))
        assertEquals(original, installed.readText())
        assertFalse(File(directory, "title.keys.tmp").exists())
    }

    @Test fun duplicatesAndOrderArePreservedForTheNativeParserToResolve() {
        val key = "0".repeat(32)
        val text = "$key=${"1".repeat(32)}\n$key=${"2".repeat(32)}\n"
        assertEquals(KeyReader.ImportResult.Success, import(text))
        assertEquals(text, installed.readText())
    }

    @Test fun trimmingAndUnterminatedInputUseTheActualNormalization() {
        val key = "0".repeat(32)
        val value = "1".repeat(32)
        assertEquals(KeyReader.ImportResult.Success, import(" $key = $value "))
        assertEquals("$key=$value\n", installed.readText())
    }

    @Test fun malformedPairDoesNotReplaceThePreviouslyImportedFile() {
        val original = "0".repeat(32) + "=" + "1".repeat(32) + "\n"
        assertEquals(KeyReader.ImportResult.Success, import(original))
        for (bad in listOf("not-a-pair\n", "a=b=c\n")) {
            assertEquals(KeyReader.ImportResult.InvalidKeys, import(bad))
            assertEquals(original, installed.readText())
        }
    }
}
