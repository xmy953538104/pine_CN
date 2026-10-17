// SPDX-License-Identifier: MPL-2.0
package emu.skyline.emulation

import android.net.Uri
import android.os.ParcelFileDescriptor
import java.io.Closeable
import java.io.FileNotFoundException
import java.io.IOException

internal class ContentOpenFailure(val source: String, val index: Int, val original: Exception) :
    IOException("Cannot open selected $source content", original) {
    val reason: String get() {
        val causes = generateSequence<Throwable>(original) { it.cause }.take(8).toList()
        return when {
            causes.any { it is SecurityException } -> "permission_denied"
            causes.any { it is FileNotFoundException } -> "file_not_found_or_unavailable"
            original is IllegalArgumentException -> "provider_argument_failure"
            else -> "io_failure"
        }
    }
}

internal class ContentFiles(val base: ParcelFileDescriptor, val dlcs: List<ParcelFileDescriptor>,
    val update: ParcelFileDescriptor?) : Closeable {
    override fun close() {
        (listOf(base) + dlcs + listOfNotNull(update)).asReversed().forEach { runCatching { it.close() } }
    }
}

/** Open in the original base/DLC/update order; transfer ownership only after all opens succeed. */
internal fun openContentFiles(base: Uri, dlcs: List<Uri>, update: Uri,
    open: (Uri) -> ParcelFileDescriptor?): ContentFiles {
    val owned = mutableListOf<ParcelFileDescriptor>()
    var source = "base"
    var index = 0
    fun descriptor(uri: Uri) = (open(uri) ?: throw FileNotFoundException("Provider returned no descriptor"))
        .also { owned.add(it) }
    try {
        val baseFile = descriptor(base)
        source = "dlc"
        val dlcFiles = dlcs.mapIndexed { position, uri -> index = position; descriptor(uri) }
        source = "update"; index = 0
        val updateFile = if (update == Uri.EMPTY) null else descriptor(update)
        return ContentFiles(baseFile, dlcFiles, updateFile)
    } catch (error: Exception) {
        owned.asReversed().forEach { runCatching { it.close() } }
        when (error) {
            is IOException, is SecurityException, is IllegalArgumentException -> throw ContentOpenFailure(source, index, error)
            else -> throw error
        }
    }
}
