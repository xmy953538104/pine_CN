// SPDX-License-Identifier: MPL-2.0
package emu.skyline.diagnostics

/** Process memory is not game-exclusive memory, NVN capacity or GPU residency.
 * No Android/guest calls here: a single worker polls this bounded model. The
 * publisher must atomically validate expected session AND launch at sink admission.
 */
internal class HostResourceSampler(
    private val clockNs: () -> Long,
    private val read: () -> Reading,
    private val publish: (Identity, Sample) -> Boolean,
    intervalMs: Long = 10_000,
    private val budgetMs: Long = 100
) {
    data class Identity(val session: String, val launch: String, val owner: Long,
        val process: String, val scope: String, val cohort: String) {
        fun valid() = session.isNotEmpty() && launch.isNotEmpty() && owner != 0L &&
            process.isNotEmpty() && scope in setOf("main", "emulation") &&
            cohort in setOf("unspecified", "official", "golden_mod")
    }
    data class Reading(val pssKib: Long?, val rssKib: Long?,
        val pssStatus: String = "observed", val rssStatus: String = "observed")
    data class Sample(val reading: Reading, val beginNs: Long, val endNs: Long,
        val overBudget: Boolean, val nextDelayMs: Long)
    enum class Result { DISABLED, NOT_DUE, BUSY, STALE, ACCEPTED, REJECTED }
    private val baseDelayMs = intervalMs.coerceIn(5_000, 60_000)
    private var delayMs = baseDelayMs
    private var identity: Identity? = null
    private var generation = 0L
    private var dueNs = 0L
    private var reading = false

    /** null disables; no read, allocation or background task is required while off. */
    @Synchronized fun update(next: Identity?) {
        val valid = next?.takeIf { it.valid() }
        if (valid == identity) return
        generation++
        identity = valid
        delayMs = baseDelayMs
        // No burst at launch/reconfiguration, and no catch-up after delayed reads.
        dueNs = clockNs() + baseDelayMs * 1_000_000
    }

    /** Only the single worker calls this after its poll has unwound. No clock,
     * allocation or retry is needed even if a query/clock failed with an Error.
     * A later explicit update may establish a fresh delayed observation epoch.
     */
    @Synchronized fun disableAfterWorkerFailure() {
        generation++
        identity = null
        reading = false
        dueNs = 0L
    }

    fun poll(): Result {
        val token: Identity
        val epoch: Long
        val begin: Long
        synchronized(this) {
            token = identity ?: return Result.DISABLED
            if (reading) return Result.BUSY
            begin = clockNs()
            if (begin < dueNs) return Result.NOT_DUE
            epoch = generation
            reading = true
        }
        // Kernel memory accounting can be slow. Never hold a lifecycle/model
        // monitor or wait for guest/render work while querying it.
        val value = try { read() } catch (_: Exception) {
            Reading(null, null, "read_failed", "read_failed")
        }
        val end = clockNs()
        val sample: Sample
        synchronized(this) {
            reading = false
            if (epoch != generation || token != identity) return Result.STALE
            val overBudget = end - begin > budgetMs * 1_000_000
            val unavailable = value.pssKib == null || value.rssKib == null
            delayMs = if (overBudget || unavailable) (delayMs * 2).coerceAtMost(60_000) else baseDelayMs
            dueNs = end + delayMs * 1_000_000
            sample = Sample(value, begin, end, overBudget, delayMs)
        }
        // update/stop may occur here: native admission is the final ownership
        // guard. Do not hold this monitor while acquiring Diagnostics/native locks.
        return if (runCatching { publish(token, sample) }.getOrDefault(false)) Result.ACCEPTED else Result.REJECTED
    }

    companion object {
        /** Strict bounded /proc/self/status field, KiB (kernel label: kB).
         * Missing/malformed data is unavailable, never a synthetic zero sample.
         */
        fun parseRssKib(status: String): Long? {
            if (status.length > 65_536) return null
            val matches = status.lineSequence().filter { it.startsWith("VmRSS:") }.take(2).toList()
            if (matches.size != 1) return null
            return Regex("VmRSS:\\s+([0-9]+)\\s+kB\\s*").matchEntire(matches.single())
                ?.groupValues?.get(1)?.toLongOrNull()
        }
    }
}

/** At most one daemon even when stop/re-enable overlaps a slow OS read. No join. */
internal class HostResourceWorker(private val sampler: HostResourceSampler) {
    private val gate = Object()
    private var enabled = false
    private var worker: Thread? = null
    fun update(identity: HostResourceSampler.Identity?) = synchronized(gate) {
        sampler.update(identity)
        enabled = identity?.valid() == true
        if (enabled && worker == null) {
            val thread = Thread({ run() }, "PineDiagMemory").apply { isDaemon = true }
            worker = thread
            try { thread.start() } catch (error: Throwable) { worker = null; enabled = false; throw error }
        }
        gate.notifyAll()
    }
    private fun run() {
        try {
            while (true) {
                synchronized(gate) {
                    if (!enabled) { worker = null; return }
                    gate.wait(5_000)
                    if (!enabled) { worker = null; return }
                }
                sampler.poll()
            }
        } catch (_: Throwable) {
            // This boundary owns only the observer daemon, never guest work.
            // Android's global uncaught handler must not turn its failure into
            // an app crash. No logging/allocation or automatic retry here.
            synchronized(gate) {
                sampler.disableAfterWorkerFailure()
                enabled = false
                worker = null
            }
        }
    }
}
