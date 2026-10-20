// SPDX-License-Identifier: MPL-2.0
package emu.skyline.diagnostics

/** One observer call per process (enforced by caller), at most three persisted reads per exit. */
internal object NativeTraceImport {
    const val MAX_ATTEMPTS = 3
    fun fields(common: Map<String, String>, trace: NativeTraceSummary.Result, attempt: Int) = common + (trace.fields() - "raw_trace_exported") +
        mapOf("trace_attempt" to attempt.toString()) // Fixed policy: raw never exported; MAX_ATTEMPTS is three.
    private fun attempts(seen: Set<String>, id: String): Int = (1..MAX_ATTEMPTS).lastOrNull { "r1:$id:$it" in seen } ?: 0
    fun eligible(seen: Set<String>, id: String): Boolean = "s1:$id" !in seen && "p1:$id" !in seen && attempts(seen, id) < MAX_ATTEMPTS
    fun attempt(seen: MutableSet<String>, id: String, read: () -> NativeTraceSummary.Result,
        writeAndFlush: (NativeTraceSummary.Result, Int) -> Boolean): Boolean {
        if (!eligible(seen, id)) return false
        val number = attempts(seen, id) + 1
        (1..MAX_ATTEMPTS).forEach { seen.remove("r1:$id:$it") }
        seen.add("r1:$id:$number")
        seen.remove("t:$id") // Old candidate markers did not distinguish negative/transient imports.
        val result = read()
        val written = writeAndFlush(result, number)
        // A null stream may also represent RemoteException. Deadlines and I/O failures
        // are not terminal. Three bounded starts still cap resources across restarts.
        if (written && (result.status == "ok" || result.status == "ownership_mismatch")) {
            seen.add((if (result.status == "ok") "s1:" else "p1:") + id)
            seen.remove("r1:$id:$number")
        }
        return true
    }
}
