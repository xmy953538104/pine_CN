// SPDX-License-Identifier: MPL-2.0
package emu.skyline.diagnostics

import android.app.ActivityManager
import android.content.Context
import android.os.Build
import java.nio.ByteBuffer
import java.util.UUID
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference

/** 84 bytes, anonymous IDs only. PID, thread names, URIs and user text are never encoded. */
internal data class ProcessAssociation(val process: String, val launch: String, val session: String,
    val commit: String, val title: String, val scope: String, val enabled: Boolean, val startObserved: Boolean)
internal object ProcessAssociationCodec {
    private const val MAGIC = 0x50445831 // PDX1
    private const val SIZE = 84
    private val uuidPattern = Regex("[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}")
    private fun hex(value: String, size: Int): ByteArray = if (value.length == size * 2 && value.all { it in "0123456789abcdefABCDEF" })
        ByteArray(size) { value.substring(it * 2, it * 2 + 2).toInt(16).toByte() } else ByteArray(size)
    fun encode(value: ProcessAssociation): ByteArray {
        val out = ByteBuffer.allocate(SIZE)
        out.putInt(MAGIC).put((if (value.enabled) 1 else 0).or(if (value.startObserved) 2 else 0).toByte())
            .put(if (value.scope == "emulation") 1.toByte() else 0.toByte()).putShort(0)
        listOf(value.process, value.launch, value.session).forEach {
            val id = if (uuidPattern.matches(it)) UUID.fromString(it) else UUID(0, 0)
            out.putLong(id.mostSignificantBits).putLong(id.leastSignificantBits)
        }
        out.put(hex(value.commit, 20)).put(hex(value.title, 8))
        return out.array()
    }
    fun decode(bytes: ByteArray?): ProcessAssociation? = runCatching {
        require(bytes != null && bytes.size == SIZE)
        val input = ByteBuffer.wrap(bytes)
        require(input.int == MAGIC)
        val flags = input.get().toInt(); val scope = input.get().toInt()
        require(flags in 0..3 && scope in 0..1 && input.short == 0.toShort())
        fun uuid(): String = UUID(input.long, input.long).let { if (it == UUID(0, 0)) "" else it.toString() }
        fun readHex(size: Int): String = ByteArray(size).also { input.get(it) }.let { data ->
            if (data.all { it == 0.toByte() }) "" else data.joinToString("") { "%02x".format(it.toInt() and 255) }
        }
        val process = uuid(); val launch = uuid(); val session = uuid()
        ProcessAssociation(process, launch, session, readHex(20), readHex(8).uppercase(java.util.Locale.ROOT),
            if (scope == 1) "emulation" else "main", flags and 1 != 0, flags and 2 != 0)
    }.getOrNull()
}

/** One daemon and one replaceable pending value, even when Binder ignores interruption. */
internal class StateSummaryPublisher(private val intervalMs: Long = 1000,
    private val publish: (ByteArray?) -> Unit, private val failed: () -> Unit = {}) {
    private data class Request(val bytes: ByteArray?)
    private val pending = AtomicReference<Request?>()
    private val scheduled = AtomicBoolean()
    private val executor = Executors.newSingleThreadScheduledExecutor { job -> Thread(job, "PineProcessSummary").apply { isDaemon = true } }
    private var lastAttempt = 0L
    private var hasPublished = false
    private var lastPublished: ByteArray? = null
    fun offer(bytes: ByteArray?) {
        require(bytes == null || bytes.size <= 128)
        pending.set(Request(bytes?.copyOf()))
        if (scheduled.compareAndSet(false, true)) schedule()
    }
    private fun schedule() {
        val delay = maxOf(0L, lastAttempt + TimeUnit.MILLISECONDS.toNanos(intervalMs) - System.nanoTime())
        executor.schedule({
            try {
                val next = pending.getAndSet(null)
                if (next != null && (!hasPublished || !java.util.Arrays.equals(lastPublished, next.bytes))) {
                    lastAttempt = System.nanoTime()
                    try { publish(next.bytes); hasPublished = true; lastPublished = next.bytes }
                    catch (_: Exception) { runCatching(failed) }
                }
            } finally {
                scheduled.set(false)
                if (pending.get() != null && scheduled.compareAndSet(false, true)) schedule()
            }
        }, delay, TimeUnit.NANOSECONDS)
    }
}

internal object ProcessStateAssociation {
    private var publisher: StateSummaryPublisher? = null
    @Synchronized fun offer(context: Context, association: ProcessAssociation) {
        if (Build.VERSION.SDK_INT < 30) return
        if (publisher == null) {
            val manager = context.applicationContext.getSystemService(ActivityManager::class.java)
            publisher = StateSummaryPublisher(publish = { manager.setProcessStateSummary(it) }, failed = {
                Diagnostics.fieldsEvent("lifecycle", "PROCESS_STATE_SUMMARY_UNAVAILABLE", "WARN") { mapOf("reason" to "platform_rejected_or_throttled") }
            })
        }
        publisher?.offer(ProcessAssociationCodec.encode(association))
    }
    @Synchronized fun clear() { publisher?.offer(null) } // A default-off process creates no thread or Binder request.
}
