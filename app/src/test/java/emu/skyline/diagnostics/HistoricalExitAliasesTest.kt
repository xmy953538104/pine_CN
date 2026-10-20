// SPDX-License-Identifier: MPL-2.0
package emu.skyline.diagnostics

import org.junit.Assert.*
import org.junit.Test

class HistoricalExitAliasesTest {
    private val old = "a".repeat(64)
    private val current = "b".repeat(64)
    private val initial = "c".repeat(64)
    @Test fun explicitAliasesCountOnceInEitherOrder() {
        for (reverse in listOf(false, true)) {
            val counter = HistoricalExitAliases(8)
            val actions = listOf<() -> Unit>({ counter.observe(old, emptyList(), true) },
                { counter.observe(current, listOf(old, initial), true) })
            (if (reverse) actions.reversed() else actions).forEach { it() }
            assertEquals(1L, counter.observations); assertEquals(3, counter.trackedIdentities)
            assertFalse(counter.limitReached)
        }
    }
    @Test fun lateStatusZeroBridgeMergesAlreadyCountedComponents() {
        val counter = HistoricalExitAliases(8)
        counter.observe(old, emptyList(), true); counter.observe(current, emptyList(), true)
        assertEquals(2L, counter.observations)
        counter.observe(current, listOf(old, initial), false)
        assertEquals(1L, counter.observations)
        counter.observe(initial, emptyList(), true); assertEquals(1L, counter.observations)
    }
    @Test fun distinctIdsNeedExplicitAliases() {
        val counter = HistoricalExitAliases(8)
        counter.observe(old, emptyList(), true); counter.observe(current, emptyList(), true)
        counter.observe(initial, listOf("A".repeat(64), "not-an-id"), false)
        assertEquals(2L, counter.observations); assertEquals(3, counter.trackedIdentities)
    }
    @Test fun chainsRepeatedAliasesAndCyclesDoNotRecountOrLoop() {
        val counter = HistoricalExitAliases(256)
        for (index in 0..127) counter.observe(index.toString(16).padStart(64, '0'),
            listOf((index + 1).toString(16).padStart(64, '0')), true)
        counter.observe("0".repeat(64), listOf(128.toString(16).padStart(64, '0'), "0".repeat(64)), true)
        assertEquals(1L, counter.observations); assertEquals(129, counter.trackedIdentities)
    }
    @Test fun identityBudgetBoundsStorageAndReportsIncompleteDedup() {
        val counter = HistoricalExitAliases(3)
        counter.observe(old, listOf(current, initial), true)
        counter.observe("d".repeat(64), listOf(old), true)
        assertEquals(3, counter.trackedIdentities); assertTrue(counter.limitReached)
        assertEquals(2L, counter.observations) // The untracked row remains an observation, not a proven unique death.
        counter.observe(current, listOf(initial), true)
        assertEquals(2L, counter.observations); assertEquals(3, counter.trackedIdentities)
    }
    @Test fun invalidCanonicalCannotUseAliasesToHideObservedRows() {
        val counter = HistoricalExitAliases(3)
        counter.observe(old, emptyList(), true)
        counter.observe(null, listOf(old), true); counter.observe("invalid", listOf(old), true)
        assertEquals(3L, counter.observations); assertEquals(1, counter.trackedIdentities)
    }
}
