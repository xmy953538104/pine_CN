// SPDX-License-Identifier: MPL-2.0
package emu.skyline.loader

import emu.skyline.diagnostics.Diagnostics
import java.lang.ref.WeakReference
import java.security.MessageDigest
import java.util.Locale

internal data class DiagnosticContentIdentity(val applicationId: String, val programId: String, val cnmtId: String,
    val parentId: String, val saveOwnerId: String, val programContentId: String, val cnmtVersion: String,
    val applicationSource: String, val observation: String = "native_metadata_scan") {
    val contentTitleId: String get() = id(cnmtId).ifEmpty { id(programId) }
    val contentTitleSource: String get() = when { id(cnmtId).isNotEmpty() -> "cnmt"; id(programId).isNotEmpty() -> "program_header"; else -> "unknown" }
    companion object {
        fun id(value: String?) = value?.uppercase(Locale.ROOT)?.takeIf { it.length == 16 && it != "0000000000000000" && it.all { c -> c in "0123456789ABCDEF" } }.orEmpty()
    }
}

/** Opt-in bounded process memory only: does not mutate AppEntry or its legacy wire format. */
internal object DiagnosticIdentityStore {
    private data class Item(val entry: WeakReference<AppEntry>, val identity: DiagnosticContentIdentity)
    private val entries = linkedMapOf<String, Item>()
    private fun key(entry: AppEntry): String = entry.format.name + ":" + MessageDigest.getInstance("SHA-256")
        .digest(entry.uri.toString().toByteArray(Charsets.UTF_8)).joinToString("") { "%02x".format(it.toInt() and 255) }
    @Synchronized fun remember(entry: AppEntry, identity: DiagnosticContentIdentity) {
        if (!Diagnostics.isActive()) return
        runCatching {
            val key = key(entry)
            entries.remove(key)
            if (entries.size == 256) entries.remove(entries.keys.first())
            entries[key] = Item(WeakReference(entry), identity)
        }
    }
    @Synchronized fun find(entry: AppEntry): DiagnosticContentIdentity? {
        if (!Diagnostics.isActive()) return null
        return runCatching {
            entries[key(entry)]?.let { item ->
                if (item.entry.get() === entry) item.identity else item.identity.copy(observation = "cached_metadata_scan_not_verified_current")
            }
        }.getOrNull()
    }
    @Synchronized fun clear() { entries.clear() }
}
internal val AppEntry.diagnosticIdentity: DiagnosticContentIdentity? get() = DiagnosticIdentityStore.find(this)
