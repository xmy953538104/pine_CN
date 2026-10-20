// SPDX-License-Identifier: MPL-2.0
package emu.skyline.diagnostics

/** Bounded explicit identity aliases for a summary count; never removes evidence rows. */
internal class HistoricalExitAliases(private val maxIdentities: Int) {
    private class Identity {
        var parent: Identity = this
        var rank = 0
        var observed = false
    }
    private val identities = hashMapOf<String, Identity>()
    var observations = 0L
        private set
    var limitReached = false
        private set
    internal val trackedIdentities get() = identities.size

    private fun valid(id: String?) = id != null && id.length == 64 && id.all { it in '0'..'9' || it in 'a'..'f' }
    private fun identity(id: String): Identity? = identities[id] ?: if (identities.size < maxIdentities) {
        Identity().also { identities[id] = it }
    } else {
        limitReached = true; null
    }
    private fun root(identity: Identity): Identity {
        var result = identity
        while (result.parent !== result) result = result.parent
        var current = identity
        while (current.parent !== current) {
            val next = current.parent
            current.parent = result
            current = next
        }
        return result
    }
    private fun merge(first: Identity, second: Identity) {
        var a = root(first)
        var b = root(second)
        if (a === b) return
        if (a.rank < b.rank) { val swap = a; a = b; b = swap }
        if (a.observed && b.observed) observations--
        a.observed = a.observed || b.observed
        b.parent = a
        if (a.rank == b.rank) a.rank++
    }
    fun observe(id: String?, aliases: List<String?>, fatal: Boolean) {
        // Without a valid canonical ID, aliases cannot establish identity.
        if (!valid(id)) { if (fatal) observations++; return }
        val current = identity(id!!) ?: run { if (fatal) observations++; return }
        for (alias in aliases) if (valid(alias)) identity(alias!!)?.let { merge(current, it) }
        val canonical = root(current)
        if (fatal && !canonical.observed) { canonical.observed = true; observations++ }
    }
}
