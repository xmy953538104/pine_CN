// SPDX-License-Identifier: MPL-2.0
package emu.skyline.diagnostics

import org.junit.Assert.*
import org.junit.Test

class LaunchIdentityTest {
    @Test fun loggingChangesDoNotTurnTheSameGameIntoAnotherLaunch() {
        val state = LaunchIdentity()
        val first = state.begin(true)
        assertEquals(36, first.length)
        assertEquals(first, state.observe(false))
        assertEquals(first, state.observe(true))
        assertTrue(state.startObserved)
        assertNotEquals(first, state.begin(true)) // Same Title/version can be another launch.
    }
    @Test fun disabledLaunchGetsNoIdUntilObservedAndDoesNotInventItsStart() {
        val state = LaunchIdentity()
        assertEquals("", state.begin(false))
        assertEquals("", state.observe(false))
        assertFalse(state.startObserved)
        assertEquals(36, state.observe(true).length)
        assertFalse(state.startObserved)
        state.end()
        assertFalse(state.active)
        assertEquals("", state.observe(true))
    }
}
