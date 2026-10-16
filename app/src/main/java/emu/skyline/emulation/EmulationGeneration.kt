// SPDX-License-Identifier: MPL-2.0
package emu.skyline.emulation

import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.withContext

/** Process-wide ownership of a game's diagnostic session and eventual process exit. */
class EmulationGeneration(
    private val worker: CoroutineDispatcher = Dispatchers.IO,
    private val completion: CoroutineDispatcher = Dispatchers.Main.immediate
) {
    private val state = Any()
    private val session = Any()
    private var generation = 0L
    private var ending = false
    private var ended = false

    /** Invalidates old completions immediately, even while their drain is running. */
    fun begin(): Long = synchronized(state) {
        generation += 1
        ending = false
        ended = false
        generation
    }

    fun isCurrent(token: Long): Boolean = synchronized(state) { token != 0L && token == generation }

    /** Call only on a worker thread: a new apply waits for any already-owned old drain. */
    fun session(token: Long, action: () -> Unit): Boolean = ownedSession(token, false, action)

    private fun canUseSession(token: Long, allowEnding: Boolean): Boolean = synchronized(state) {
        token != 0L && token == generation && !ended && (allowEnding || !ending)
    }

    private fun ownedSession(token: Long, allowEnding: Boolean, action: () -> Unit): Boolean = synchronized(session) {
        if (!canUseSession(token, allowEnding)) false else {
            action()
            canUseSession(token, allowEnding)
        }
    }

    /** Once accepted, caller/Activity cancellation must not discard normal process ending. */
    suspend fun finish(token: Long, drain: () -> Unit, exit: () -> Unit) = withContext(NonCancellable) {
        val accepted = synchronized(state) {
            if (token == 0L || token != generation || ending || ended) false else {
                ending = true
                true
            }
        }
        if (accepted) {
            withContext(worker) { ownedSession(token, true, drain) }
            withContext(completion) {
                synchronized(state) {
                    if (token == generation && !ended) {
                        ended = true
                        exit()
                    }
                }
            }
        }
    }
}
